#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

///! Session and power actions (lock, log off, shut down, reboot, suspend, hibernate).
/// Replaces spawning `rundll32`/`shutdown` child processes from QML.
class Session: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	/// If the system has a hibernation file and can hibernate.
	Q_PROPERTY(bool canHibernate READ canHibernate CONSTANT);

public:
	explicit Session(QObject* parent = nullptr): QObject(parent) {}

	/// Locks the current session (`LockWorkStation`).
	Q_INVOKABLE static void lock();
	/// Logs the current user off (`ExitWindowsEx(EWX_LOGOFF)`).
	Q_INVOKABLE static void logout();
	/// Shuts the machine down (`InitiateShutdownW(SHUTDOWN_POWEROFF)`).
	Q_INVOKABLE static void shutdown();
	/// Reboots the machine (`InitiateShutdownW(SHUTDOWN_RESTART)`).
	Q_INVOKABLE static void reboot();
	/// Suspends the machine to RAM (`SetSuspendState(hibernate=false)`).
	Q_INVOKABLE static void suspend();
	/// Hibernates the machine (`SetSuspendState(hibernate=true)`).
	Q_INVOKABLE static void hibernate();
	/// Reboots directly into UEFI/BIOS firmware setup.
	Q_INVOKABLE static void rebootToFirmware();

	[[nodiscard]] static bool canHibernate();
};

} // namespace qs::windows::sys
