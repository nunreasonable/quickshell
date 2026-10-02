#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::windows::image {

///! Generates freedesktop-layout thumbnail PNGs for the wallpaper selector grid
/// (modules/ii/wallpaperSelector/**, via ThumbnailImage.qml), replacing the `magick` CLI call
/// ThumbnailImage.qml shells out to on Linux (scripts/thumbnails/generate-thumbnails-magick.sh).
///
/// The actual cache path (`<thumbnail cache>/<size>/<md5(file://url)>.png`) stays computed in
/// QML: `Qt.md5()` is a plain QtQml builtin that works the same on Windows, so only the decode
/// + scale + encode step needs a native replacement here.
class Thumbnailer: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Thumbnailer(QObject* parent = nullptr): QObject(parent) {}

	/// Starts generating a thumbnail of `sourcePath`, scaled to fit within `maxSize`x`maxSize`
	/// (preserving aspect ratio, same as ImageMagick's plain `-resize WxH`), written to
	/// `outputPath` as a PNG. Runs on a QThreadPool worker thread - decoding a full-resolution
	/// wallpaper is too slow for the GUI thread, which is also why the Linux side shells out
	/// to `magick` instead of doing it inline.
	///
	/// If `outputPath` already exists, `finished` is emitted immediately (on the calling
	/// thread) with `ok=true` and nothing is regenerated - same "skip if present" contract
	/// ThumbnailImage.qml's bash one-liner has.
	Q_INVOKABLE void generate(const QString& sourcePath, const QString& outputPath, int maxSize);

signals:
	/// Emitted (queued to this object's thread, i.e. the GUI thread) when a `generate()` call
	/// finishes. `ok` is false if `sourcePath` couldn't be decoded or `outputPath` couldn't be
	/// written.
	void finished(const QString& sourcePath, const QString& outputPath, bool ok);
};

} // namespace qs::windows::image
