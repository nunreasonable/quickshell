#include "fs_utils.hpp"

#include <qt_windows.h>

#include <qdir.h>

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

} // namespace qs::windows::sys
