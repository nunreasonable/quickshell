#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class Screenshot: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Screenshot(QObject* parent = nullptr): QObject(parent) {}

	Q_INVOKABLE bool captureScreen(const QString& screenName, const QString& path);

	Q_INVOKABLE bool
	cropToFile(const QString& srcPath, int x, int y, int width, int height, const QString& dstPath);
};

} // namespace qs::windows::sys
