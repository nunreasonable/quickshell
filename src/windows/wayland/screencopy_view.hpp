#pragma once

#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qsgnode.h>
#include <qsize.h>
#include <qtmetamacros.h>

#include "../capture.hpp"
#include "toplevel.hpp"

// Windows implementation of Quickshell.Wayland's ScreencopyView on Windows.Graphics.Capture,
// same QML API as the wayland one (src/wayland/screencopy/view.hpp). Same namespace so the
// documentation lines up.
namespace qs::wayland::screencopy {

///! Displays a video stream from other windows or a monitor.
/// ScreencopyView displays live video streams or single captured frames from valid
/// capture sources. See @@captureSource for details on which objects are accepted.
///
/// On Windows this is backed by Windows.Graphics.Capture. Minimized windows produce no
/// frames: the view keeps showing the last frame it got (if any) and @@hasContent is false
/// until the window is restored.
class ScreencopyView: public QQuickItem {
	Q_OBJECT;
	QML_ELEMENT;
	// clang-format off
	/// The object to capture from. Accepts any of the following:
	/// - `null` - Clears the displayed image.
	/// - @@Quickshell.ShellScreen - A monitor.
	/// - @@Quickshell.Wayland.Toplevel - A toplevel window.
	Q_PROPERTY(QObject* captureSource READ captureSource WRITE setCaptureSource NOTIFY captureSourceChanged);
	/// If true, the system cursor will be painted on the image. Defaults to false.
	Q_PROPERTY(bool paintCursor READ paintCursors WRITE setPaintCursors NOTIFY paintCursorsChanged);
	/// If true, a live video feed from the capture source will be displayed instead of a still image.
	/// Defaults to false.
	///
	/// Live feeds are throttled to 30 fps and paused while the view (or its window) is not visible.
	Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged);
	/// If true, the view has content ready to display. Content is not always immediately available,
	/// and this property can be used to avoid displaying it until ready.
	Q_PROPERTY(bool hasContent READ default NOTIFY hasContentChanged BINDABLE bindableHasContent);
	/// The size of the source image in device pixels. Valid when @@hasContent is true.
	Q_PROPERTY(QSize sourceSize READ default NOTIFY sourceSizeChanged BINDABLE bindableSourceSize);
	/// If nonzero, the width and height constraints set for this property will constrain those
	/// dimensions of the ScreencopyView's implicit size, maintaining the image's aspect ratio.
	Q_PROPERTY(QSizeF constraintSize READ default WRITE default NOTIFY constraintSizeChanged BINDABLE bindableConstraintSize);
	// clang-format on

public:
	explicit ScreencopyView(QQuickItem* parent = nullptr);
	~ScreencopyView() override;
	Q_DISABLE_COPY_MOVE(ScreencopyView);

	void componentComplete() override;

	/// Capture a single frame. Has no effect if @@live is true.
	Q_INVOKABLE void captureFrame();

	[[nodiscard]] QObject* captureSource() const { return this->mCaptureSource; }
	void setCaptureSource(QObject* captureSource);

	[[nodiscard]] bool paintCursors() const { return this->mPaintCursors; }
	void setPaintCursors(bool paintCursors);

	[[nodiscard]] bool live() const { return this->mLive; }
	void setLive(bool live);

	[[nodiscard]] QBindable<bool> bindableHasContent() { return &this->bHasContent; }
	[[nodiscard]] QBindable<QSize> bindableSourceSize() { return &this->bSourceSize; }
	[[nodiscard]] QBindable<QSizeF> bindableConstraintSize() { return &this->bConstraintSize; }

	// Read by the paint node on the render thread (the GUI thread is blocked then).
	[[nodiscard]] qs::windows::capture::CaptureHandle* handle() const { return this->mHandle; }
	[[nodiscard]] bool hasFrame() const { return this->mHasFrame; }

signals:
	/// The capture source ended the stream (its window was closed or capture failed).
	void stopped();

	void captureSourceChanged();
	void paintCursorsChanged();
	void liveChanged();
	void hasContentChanged();
	void sourceSizeChanged();
	void constraintSizeChanged();

protected:
	QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
	void itemChange(ItemChange change, const ItemChangeData& value) override;

private slots:
	void onCaptureSourceDestroyed();
	void onFrameReady();
	void onCaptureStopped(bool error);

private:
	void createContext();
	void destroyContext(bool update = true);
	// Starts or stops the capture session from the current state (visibility, live, ...).
	void syncSession();
	void updateImplicitSize();
	void watchWindow(QQuickWindow* window);

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(ScreencopyView, bool, bHasContent, &ScreencopyView::hasContentChanged);
	Q_OBJECT_BINDABLE_PROPERTY(ScreencopyView, QSize, bSourceSize, &ScreencopyView::sourceSizeChanged);
	Q_OBJECT_BINDABLE_PROPERTY(ScreencopyView, QSizeF, bConstraintSize, &ScreencopyView::constraintSizeChanged);
	Q_OBJECT_BINDABLE_PROPERTY(ScreencopyView, QSizeF, bImplicitSize, &ScreencopyView::updateImplicitSize);
	// clang-format on

	QObject* mCaptureSource = nullptr;
	toplevel::Toplevel* toplevel = nullptr;
	bool mPaintCursors = false;
	bool mLive = false;
	bool completed = false;
	// A single frame is wanted (initial capture or captureFrame()); cleared once it arrives.
	bool wantFrame = false;
	// A frame is in the paint node, so it has something to show even if hasContent is false.
	bool mHasFrame = false;
	qs::windows::capture::CaptureHandle* mHandle = nullptr;
	QQuickWindow* watchedWindow = nullptr;
};

} // namespace qs::wayland::screencopy
