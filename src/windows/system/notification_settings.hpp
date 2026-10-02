#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

///! Opens the Windows settings pages that govern notifications.
/// Windows keeps showing its own toast banners even while the shell shows its popups for the
/// same notifications (no API lets another app suppress them). To see only the shell's popups,
/// the user can turn "Show notification banners" off per app, or turn on Do not disturb, on the
/// page @@openSettings() opens; the shell never changes those settings itself. Toasts still land
/// in the notification center with banners off, so Quickshell.Services.Notifications keeps
/// mirroring them.
class NotificationSettings: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit NotificationSettings(QObject* parent = nullptr): QObject(parent) {}

	/// Opens Settings > System > Notifications (`ms-settings:notifications`): Do not disturb and
	/// per-app banner/sound settings.
	Q_INVOKABLE static void openSettings();
	/// Opens Settings > Privacy & security > Notifications (`ms-settings:privacy-notifications`):
	/// the switch that lets apps, the shell included, read the notification center.
	Q_INVOKABLE static void openAccessSettings();
};

} // namespace qs::windows::sys
