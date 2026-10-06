#pragma once

#include <qdatetime.h>
#include <qimage.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qtypes.h>

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

	Q_INVOKABLE QString pixelAt(const QString& path, int x, int y);
	Q_INVOKABLE void releasePixels();

private:
	bool loadPixels(const QString& path);

	QString pixelsPath;
	QDateTime pixelsModified;
	qint64 pixelsFileSize = -1;
	QImage pixels;
};

} // namespace qs::windows::sys
