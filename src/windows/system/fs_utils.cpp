#include "fs_utils.hpp"

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qstandardpaths.h>

namespace qs::windows::sys {

QString FsUtils::classify(const QString& path) {
	auto wpath = path.toStdWString();
	auto attrs = GetFileAttributesW(wpath.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES) return QStringLiteral("invalid");
	return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? QStringLiteral("dir") : QStringLiteral("file");
}

QStringList FsUtils::listDir(const QString& path) {
	return QDir(path).entryList(QDir::Files, QDir::Name);
}

QString FsUtils::findExecutable(const QString& name) {
	if (name.isEmpty()) return QString();

	auto local = QFileInfo(QDir(QCoreApplication::applicationDirPath()), name);
	if (local.isFile()) return local.absoluteFilePath();

	// Tries PATHEXT's extensions when `name` has none.
	return QStandardPaths::findExecutable(name);
}

} // namespace qs::windows::sys
