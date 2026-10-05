#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <qt_windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtypes.h>
#include <winrt/base.h>

namespace qs::windows::recorder {

///! What the default output device plays, through WASAPI loopback, as 16-bit stereo PCM.
/// Loopback delivers nothing while nothing plays, so the gaps are filled with silence from the
/// packets' QPC positions: the stream stays continuous and in step with the video clock, which
/// the AAC encoder and the MP4 muxer both expect. The conversion (any shared mode mix format to
/// 16-bit stereo at 44.1 or 48 kHz, the rates Media Foundation's AAC encoder takes) is done here
/// instead of trusting AUTOCONVERTPCM, which loopback streams don't reliably honor.
///
/// open() and start() on an MTA thread; capture itself runs on a thread of its own.
class LoopbackCapture {
public:
	LoopbackCapture() = default;
	~LoopbackCapture();
	Q_DISABLE_COPY_MOVE(LoopbackCapture);

	bool open(QString* error);
	[[nodiscard]] int sampleRate() const { return this->outRate; }
	static constexpr int CHANNELS = 2;

	// Starts capturing; sample 0 of the output is at `origin` (qpc100ns()).
	void start(qint64 origin);
	void stop();

	// Moves out everything converted so far (interleaved stereo), contiguous with what the
	// previous call returned.
	std::vector<qint16> take();

	// The capture thread lost the device (IAudioClient::Start failed, or it was invalidated
	// mid-capture); the rest of the recording gets silence. Safe to poll from another thread.
	[[nodiscard]] bool lost() const { return this->mLost.load(std::memory_order_relaxed); }

private:
	enum class SampleType : quint8 { Float32, Int16, Int24, Int32 };

	void run();
	void drain();
	void consume(const BYTE* data, UINT32 frames, DWORD flags, UINT64 qpcPosition);
	void appendSilenceLocked(qint64 frames);
	[[nodiscard]] qint64 framesAt(qint64 time) const;

	winrt::com_ptr<IAudioClient> client;
	winrt::com_ptr<IAudioCaptureClient> capture;
	std::thread thread;
	HANDLE stopEvent = nullptr;
	qint64 origin = 0;

	// Input format.
	SampleType type = SampleType::Float32;
	int inChannels = 0;
	int inRate = 0;
	int bytesPerSample = 0;
	// Per input channel weights into the left/right output channels (a plain downmix).
	std::vector<std::array<float, 2>> weights;

	int outRate = 0;
	// Linear resampler state, used only when the mix rate isn't one the AAC encoder takes.
	double resampleStep = 1.0;
	double resamplePos = 0.0;
	std::array<float, 2> resamplePrev {};
	std::vector<float> scratch; // input converted to float stereo

	std::atomic<bool> mLost {false};

	std::mutex mutex;
	std::vector<qint16> pending;
	qint64 written = 0; // output frames produced (pending + taken)
};

} // namespace qs::windows::recorder
