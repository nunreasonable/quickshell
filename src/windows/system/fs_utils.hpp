#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

///! Tiny filesystem helpers. classify() is for services/Wallpapers.qml's directory navigation (address bar,
/// clipboard paste, random-pick-landed-on-a-subfolder): those need to know whether an arbitrary
/// path is a file or a directory before deciding to browse into it or apply it as a wallpaper,
/// and Windows has no `test`/`bash` to shell out to for the one-line stat the Linux side runs.
class FsUtils: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit FsUtils(QObject* parent = nullptr): QObject(parent) {}

	/// Classifies `path` (`GetFileAttributesW`) as `"dir"`, `"file"`, or `"invalid"` if it
	/// doesn't exist - the same three outcomes the Linux side's `[ -d ]`/`[ -f ]` one-liner
	/// produces.
	Q_INVOKABLE static QString classify(const QString& path);

	/// `ls -1 <path>`: names of the files in `path` (no directories, no dot entries), sorted.
	/// Empty if it doesn't exist. Unlike `cmd /c dir /b` this keeps non-ASCII names intact.
	Q_INVOKABLE static QStringList listDir(const QString& path);
};

} // namespace qs::windows::sys
