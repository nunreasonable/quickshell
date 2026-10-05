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

	/// `command -v <name>` for the helper programs ii runs (songrec.exe...): `name` next to
	/// qs.exe first, where the package stages its optional tools, then on PATH. Returns the
	/// absolute path with forward slashes, or an empty string if it is nowhere to be found -
	/// so callers can say what's missing instead of Process failing to start silently.
	Q_INVOKABLE static QString findExecutable(const QString& name);
};

} // namespace qs::windows::sys
