#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class Wallpaper: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Wallpaper(QObject* parent = nullptr): QObject(parent) {}

	Q_INVOKABLE static QString currentWallpaper();

	Q_INVOKABLE static bool setWallpaper(const QString& path);

	Q_INVOKABLE static bool isDarkMode();

	Q_INVOKABLE static void setDarkMode(bool dark);

	Q_INVOKABLE static bool setAccentColor(const QString& hex);

	Q_INVOKABLE static QString matugenPath();
};

} // namespace qs::windows::sys
