#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows::services::notifications {

///! Whether the shell may read the Windows notification center (Windows only).
/// See @@NotificationServer.systemNotificationAccess.
class SystemNotificationAccess: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		/// Not asked yet (the mirror hasn't started).
		Unknown = 0,
		/// Windows hasn't recorded a decision for the shell.
		Unspecified = 1,
		/// The shell can read and remove toasts.
		Allowed = 2,
		/// Notification access is turned off in Settings > Privacy & security > Notifications.
		Denied = 3,
		/// The listener API itself failed (not available on this Windows build).
		Unavailable = 4,
	};
	Q_ENUM(Enum);

	Q_INVOKABLE static QString
	toString(qs::windows::services::notifications::SystemNotificationAccess::Enum value);
};

// A toast as read from the Windows notification center, already reduced to what a
// Notification needs. Built on the listener thread, consumed on the GUI thread.
struct ToastSnapshot {
	quint32 id = 0;
	QString aumid;
	QString appName;
	QString logoPath; // absolute path of the app logo cached as PNG, or empty
	QString summary;  // first text element
	QString body;     // the other text elements, one per line, plain text
};

class ToastMirrorWorker;

///! GUI-thread owner of the UserNotificationListener thread.
/// Windows has no way for a shell to replace the toast UI, but the notification listener lets
/// an app read (and remove) what's in the notification center. Runs the listener on its own
/// MTA thread (Qt's GUI thread is STA; see docs/AGENTS.md) and reports new and removed toasts
/// through signals on the GUI thread.
///
/// NotificationChanged needs package identity (it throws 0x80070490 for an unpackaged exe on
/// 24H2+), so without it the worker polls GetNotificationsAsync every 2 s and diffs by toast id.
/// Toasts already present when mirroring starts are only remembered, never re-announced.
class ToastMirror: public QObject {
	Q_OBJECT;

public:
	explicit ToastMirror(QObject* parent = nullptr);
	~ToastMirror() override;
	Q_DISABLE_COPY_MOVE(ToastMirror);

	// Each of these posts to the worker and returns immediately.
	void setEnabled(bool enabled);
	void requestAccess();
	void removeToast(quint32 id);

	[[nodiscard]] SystemNotificationAccess::Enum access() const { return this->mAccess; }
	[[nodiscard]] bool isActive() const { return this->mActive; }

signals:
	void toastAdded(const qs::windows::services::notifications::ToastSnapshot& toast);
	void toastRemoved(quint32 id);
	void accessChanged();
	void activeChanged();

private:
	friend class ToastMirrorWorker;

	// Called (queued) by the worker.
	void workerState(SystemNotificationAccess::Enum access, bool active);

	QThread mThread;
	ToastMirrorWorker* mWorker = nullptr;
	SystemNotificationAccess::Enum mAccess = SystemNotificationAccess::Unknown;
	bool mActive = false;
};

} // namespace qs::windows::services::notifications
