#pragma once

#include <memory>
#include <mutex>

#include <qt_windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <qimage.h>
#include <qobject.h>
#include <qsize.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtmetamacros.h>
#include <qtypes.h>

// Windows.Graphics.Capture backend shared by ScreencopyView (live previews) and the
// Screenshot singleton. Everything WinRT happens on one process wide MTA thread
// (CaptureThread) that also owns a dedicated D3D11 device the frame pools allocate on.
//
// Frames reach consumers (Qt's render thread) without CPU copies: the capture side copies each
// frame into a texture created with a shared NT handle and a keyed mutex (SharedFrame), and the
// consumer opens that handle on its own D3D11 device. The keyed mutex orders the two devices'
// GPU work: the producer acquires key 0, writes, releases key 1; the consumer acquires 1, reads,
// releases 0. A separate device (instead of using Qt's) keeps capture independent from Qt's
// per window RHI lifecycle and from the render thread, which is the only thread allowed to
// touch Qt's device.
namespace qs::windows::capture {

class CaptureSession;
class CaptureWorker;
struct SessionCore;

struct CaptureTarget {
	HWND window = nullptr;
	HMONITOR monitor = nullptr;

	[[nodiscard]] bool valid() const { return this->window != nullptr || this->monitor != nullptr; }
};

struct CaptureOptions {
	bool cursor = false;
	// Keep delivering frames (throttled to maxFps). Otherwise the session stops after one.
	bool live = false;
	int maxFps = 30;
};

// A frame published by the capture side: an RGBA8 texture on the capture device, exported as
// an NT handle. Consumers open `handle` on their device (SharedFrameReader).
struct SharedFrame {
	~SharedFrame();
	Q_DISABLE_COPY_MOVE(SharedFrame);
	SharedFrame() = default;

	QSize size;
	HANDLE handle = nullptr;
	ID3D11Texture2D* texture = nullptr;
	IDXGIKeyedMutex* mutex = nullptr;
};

// Consumer side of a SharedFrame on a different D3D11 device. `copyTo` is the only method that
// enqueues GPU work; call it from the thread that owns `context`.
class SharedFrameReader {
public:
	SharedFrameReader() = default;
	~SharedFrameReader();
	Q_DISABLE_COPY_MOVE(SharedFrameReader);

	// Opens `frame` on `device`. Replaces the previously opened frame.
	bool open(ID3D11Device* device, const std::shared_ptr<SharedFrame>& frame);
	void close();

	[[nodiscard]] const std::shared_ptr<SharedFrame>& frame() const { return this->mFrame; }

	// Copies the shared content into `dest` (same size/format, on `device`). Returns false
	// without copying if the producer still holds the keyed mutex.
	bool copyTo(ID3D11DeviceContext* context, ID3D11Texture2D* dest);

private:
	std::shared_ptr<SharedFrame> mFrame;
	ID3D11Texture2D* opened = nullptr;
	IDXGIKeyedMutex* mutex = nullptr;
};

///! GUI thread facade of one capture session.
/// Owns a CaptureSession living on the capture thread. The latest frame is readable from any
/// thread through `latestFrame()`; `frameReady` is delivered on this object's thread.
class CaptureHandle: public QObject {
	Q_OBJECT;

public:
	explicit CaptureHandle(
	    const CaptureTarget& target,
	    const CaptureOptions& options,
	    QObject* parent = nullptr
	);
	~CaptureHandle() override;
	Q_DISABLE_COPY_MOVE(CaptureHandle);

	// (Re)starts capture; idempotent while running. A non live session stops by itself after
	// its first frame, after which start() grabs another one.
	void start();
	void stop();
	[[nodiscard]] bool running() const { return this->mRunning; }

	void setCursor(bool cursor);
	void setLive(bool live);

	// Latest published frame and its serial (increases with every new frame). Thread safe.
	[[nodiscard]] std::shared_ptr<SharedFrame> latestFrame(quint64* serial = nullptr) const;

signals:
	// A new frame was published (coalesced: at most one pending notification per frame).
	void frameReady();
	// The source went away (window closed, monitor removed) or capture failed to start.
	void stopped(bool error);

private:
	friend class CaptureSession;
	void onSessionStopped(bool error);
	void onFrame();

	std::shared_ptr<SessionCore> core;
	CaptureSession* session = nullptr;
	bool mRunning = false;
	bool live = false;
};

///! Process wide capture thread (MTA apartment + D3D11 device).
class CaptureThread: public QObject {
	Q_OBJECT;

public:
	static CaptureThread* instance();

	[[nodiscard]] CaptureWorker* worker() const { return this->mWorker; }
	[[nodiscard]] QThread* thread() { return &this->mThread; }

	// Captures one frame of a monitor (no cursor) into an RGBA8888 QImage. Blocks the calling
	// thread (not a COM wait, so it is fine on the GUI thread) for at most `timeoutMs`.
	// Returns a null image on failure.
	[[nodiscard]] QImage grabMonitor(HMONITOR monitor, int timeoutMs = 2000);

private:
	explicit CaptureThread();
	~CaptureThread() override;
	Q_DISABLE_COPY_MOVE(CaptureThread);

	QThread mThread;
	CaptureWorker* mWorker = nullptr;
};

} // namespace qs::windows::capture
