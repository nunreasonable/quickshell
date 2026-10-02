#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

///! Still screen captures (replaces grim/hyprshot for the screenshot and OCR flows).
/// Uses the same Windows.Graphics.Capture backend as @@Quickshell.Wayland.ScreencopyView,
/// reading the frame back to the CPU.
class Screenshot: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Screenshot(QObject* parent = nullptr): QObject(parent) {}

	/// Captures a screen to a PNG file at `path` (directories are created) and returns true on
	/// success. `screenName` is a @@Quickshell.ShellScreen's `name`; an empty name captures
	/// every screen into one image laid out like the desktop. The cursor is not included.
	///
	/// Blocks the caller until the frame is read back, typically well under 100 ms.
	Q_INVOKABLE bool captureScreen(const QString& screenName, const QString& path);

	/// Crops `srcPath` to the device-pixel rect `(x, y, width, height)` and saves the result as
	/// a PNG at `dstPath` (directories are created), returning true on success. `x`/`y` are
	/// clamped into the source image, same as ImageMagick's `-crop` would after a `+repage`.
	/// Replaces the region selector's `magick ... -crop WxH+X+Y` step.
	Q_INVOKABLE bool
	cropToFile(const QString& srcPath, int x, int y, int width, int height, const QString& dstPath);
};

} // namespace qs::windows::sys
