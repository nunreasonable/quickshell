#pragma once

#include <qobject.h>
#include <qsize.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qvariant.h>

namespace qs::windows::image {

class ImageTools: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit ImageTools(QObject* parent = nullptr): QObject(parent) {}

	Q_INVOKABLE static QVariantMap leastBusyRegion(
	    const QString& imagePath,
	    int width,
	    int height,
	    int screenWidth,
	    int screenHeight,
	    int horizontalPadding,
	    int verticalPadding,
	    bool busiest
	);

	Q_INVOKABLE static QVariantMap textColorFromImage(const QString& imagePath);

	Q_INVOKABLE static QString schemeForImage(const QString& imagePath);

	Q_INVOKABLE static QSize imageSize(const QString& imagePath);
};

} // namespace qs::windows::image
