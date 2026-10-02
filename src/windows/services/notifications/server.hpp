#pragma once

#include <windows.h>

#include <functional>

#include <qcontainerfwd.h>
#include <qhash.h>
#include <qobject.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../../../core/model.hpp"
#include "notification.hpp"
#include "toast_mirror.hpp"

namespace qs::windows::services::notifications {

struct NotificationServerSupport {
	bool persistence = false;
	bool body = true;
	bool bodyMarkup = false;
	bool bodyHyperlinks = false;
	bool bodyImages = false;
	bool actions = false;
	bool actionIcons = false;
	bool image = false;
	bool inlineReply = false;
	QVector<QString> extraHints;
};

// Process wide notification store, the Windows counterpart of the Linux D-Bus server.
// Notifications come from the shell itself (notifySend), from other qs processes of the same
// config through the session bus window (forwardToOwner) and from the Windows notification
// center (ToastMirror).
class NotificationServer: public QObject {
	Q_OBJECT;

public:
	static NotificationServer* instance();

	void switchGeneration(bool reEmit, const std::function<void()>& clearHook);
	ObjectModel<Notification>* trackedNotifications();
	void deleteNotification(Notification* notification, NotificationCloseReason::Enum reason);

	// Same contract as the D-Bus Notify method: creates a notification (or updates the one with
	// `replacesId`), emits `notification` for new ones and returns the id.
	quint32 notify(
	    const QString& appName,
	    quint32 replacesId,
	    const QString& appIcon,
	    const QString& summary,
	    const QString& body,
	    const QStringList& actions,
	    const QVariantMap& hints,
	    qint32 expireTimeout
	);

	// What `notify-send summary body args...` does on Linux. The id is handed out right away but
	// the notification arrives on the next event loop turn, as it would after the D-Bus round
	// trip, so callers never re-enter their own notification handlers. Sends made before any
	// NotificationServer is live (during config load) wait for the next switchGeneration.
	quint32 notifySend(const QString& summary, const QString& body, const QStringList& args);

	void invokeAction(Notification* notification, const QString& identifier);

	void setMirrorEnabled(bool enabled);
	void requestSystemAccess();
	[[nodiscard]] SystemNotificationAccess::Enum systemAccess() const;
	[[nodiscard]] bool isSystemMirrorActive() const;

	NotificationServerSupport support;

signals:
	void notification(qs::windows::services::notifications::Notification* notification);
	void actionInvoked(quint32 id, const QString& action);
	void systemAccessChanged();
	void systemMirrorActiveChanged();

private:
	explicit NotificationServer() = default;

	quint32 deliver(
	    quint32 reservedId,
	    quint32 replacesId,
	    const QString& appName,
	    const QString& appIcon,
	    const QString& summary,
	    const QString& body,
	    const QStringList& actions,
	    const QVariantMap& hints,
	    qint32 expireTimeout,
	    quint32 toastId = 0,
	    const QString& aumid = QString()
	);

	[[nodiscard]] bool hasReceiver() const;

	// Like the org.freedesktop.Notifications bus name, one process per session owns the
	// notifications: the first shell.qml instance. Others (settings, welcome, dialogs started
	// from the same config) forward notifySend to it and don't mirror Windows toasts.
	void claimSession();
	quint32 forwardToOwner(const QString& summary, const QString& body, const QStringList& args);
	static LRESULT CALLBACK busWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	bool mOwner = false;
	void* ownerMutex = nullptr;
	HWND busWindow = nullptr;

	ToastMirror* mirror();
	void onToastAdded(const ToastSnapshot& toast);
	void onToastRemoved(quint32 toastId);
	static void activateApp(const QString& aumid, const QString& desktopEntry);

	quint32 nextId = 1;
	QHash<quint32, Notification*> idMap;
	QHash<quint32, Notification*> toastMap; // listener toast id -> its mirror
	ObjectModel<Notification> mNotifications {this};
	ToastMirror* mMirror = nullptr;
	QList<std::function<void()>> pendingSends; // notifySend calls made with no receiver yet
};

} // namespace qs::windows::services::notifications
