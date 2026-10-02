#include "server.hpp"
#include <functional>

#include <qcontainerfwd.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qqmlengine.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../../../core/desktopentry.hpp"
#include "../../../core/logcat.hpp"
#include "../../../core/model.hpp"
#include "../../desktopentry_backend.hpp"
#include "../../window_tracker.hpp"
#include "notification.hpp"
#include "notify_send.hpp"
#include "toast_mirror.hpp"

namespace qs::windows::services::notifications {

// NOLINTNEXTLINE(misc-use-internal-linkage)
QS_LOGGING_CATEGORY(logNotifications, "quickshell.windows.notifications", QtWarningMsg);

NotificationServer* NotificationServer::instance() {
	static auto* instance = new NotificationServer(); // NOLINT
	return instance;
}

void NotificationServer::switchGeneration(bool reEmit, const std::function<void()>& clearHook) {
	auto notifications = this->mNotifications.valueList();
	this->mNotifications.valueList().clear();
	this->idMap.clear();
	this->toastMap.clear();

	clearHook();

	if (reEmit) {
		for (auto* notification: notifications) {
			notification->setLastGeneration();
			notification->setTracked(false);
			emit this->notification(notification);

			if (!notification->isTracked()) {
				delete notification;
			} else {
				this->idMap.insert(notification->id(), notification);
				if (notification->toastId() != 0) this->toastMap.insert(notification->toastId(), notification);
				this->mNotifications.insertObject(notification);
			}
		}
	} else {
		// Expired, not dismissed: mirrored toasts stay in the Windows notification center.
		for (auto* notification: notifications) {
			delete notification;
		}
	}
}

ObjectModel<Notification>* NotificationServer::trackedNotifications() {
	return &this->mNotifications;
}

void NotificationServer::deleteNotification(
    Notification* notification,
    NotificationCloseReason::Enum reason
) {
	if (!this->idMap.contains(notification->id())) return;

	emit notification->closed(reason);

	this->mNotifications.removeObject(notification);
	this->idMap.remove(notification->id());

	if (auto toastId = notification->toastId(); toastId != 0) {
		this->toastMap.remove(toastId);

		// Dismissed here means the user is done with it, same as dismissing it in Windows' own
		// notification center. Expiring (popup timeouts, reloads) leaves the toast alone, and
		// CloseRequested means it is already gone from Windows.
		if (reason == NotificationCloseReason::Dismissed && this->mMirror) {
			this->mMirror->removeToast(toastId);
		}
	}

	notification->retainedDestroy();
}

quint32 NotificationServer::notify(
    const QString& appName,
    quint32 replacesId,
    const QString& appIcon,
    const QString& summary,
    const QString& body,
    const QStringList& actions,
    const QVariantMap& hints,
    qint32 expireTimeout
) {
	return this->deliver(0, replacesId, appName, appIcon, summary, body, actions, hints, expireTimeout);
}

quint32 NotificationServer::notifySend(
    const QString& summary,
    const QString& body,
    const QStringList& args
) {
	auto request = parseNotifySendArgs(args);

	auto replacing = request.replacesId != 0 && this->idMap.contains(request.replacesId);
	auto reservedId = replacing ? 0 : this->nextId++;

	QMetaObject::invokeMethod(
	    this,
	    [this, reservedId, request, summary, body] {
		    this->deliver(
		        reservedId,
		        request.replacesId,
		        request.appName,
		        request.appIcon,
		        summary,
		        body,
		        request.actions,
		        request.hints,
		        request.expireTimeout
		    );
	    },
	    Qt::QueuedConnection
	);

	return replacing ? request.replacesId : reservedId;
}

quint32 NotificationServer::deliver(
    quint32 reservedId,
    quint32 replacesId,
    const QString& appName,
    const QString& appIcon,
    const QString& summary,
    const QString& body,
    const QStringList& actions,
    const QVariantMap& hints,
    qint32 expireTimeout,
    quint32 toastId,
    const QString& aumid
) {
	auto* notification = replacesId == 0 ? nullptr : this->idMap.value(replacesId);
	auto old = notification != nullptr;

	if (!notification) {
		notification = new Notification(reservedId != 0 ? reservedId : this->nextId++, this);
		QQmlEngine::setObjectOwnership(notification, QQmlEngine::CppOwnership);
		if (toastId != 0) notification->setSystemToast(toastId, aumid);
	}

	notification->updateProperties(appName, appIcon, summary, body, actions, hints, expireTimeout);

	if (!old) {
		emit this->notification(notification);

		if (!notification->isTracked()) {
			auto id = notification->id();
			delete notification;
			return id;
		}

		this->idMap.insert(notification->id(), notification);
		if (toastId != 0) this->toastMap.insert(toastId, notification);
		this->mNotifications.insertObject(notification);
	}

	return notification->id();
}

void NotificationServer::invokeAction(Notification* notification, const QString& identifier) {
	// A toast's own buttons and launch arguments only reach the app through Windows' toast
	// activation, which another process can't trigger; bringing up the app is the best stand-in.
	if (notification->toastId() != 0 && identifier == QStringLiteral("default")) {
		NotificationServer::activateApp(
		    notification->aumid(),
		    notification->bindableDesktopEntry().value()
		);
	}

	emit this->actionInvoked(notification->id(), identifier);
}

void NotificationServer::activateApp(const QString& aumid, const QString& desktopEntry) {
	if (aumid.isEmpty()) return;

	// Packaged apps and Win32 apps that set an explicit AUMID on their windows report it as the
	// tracker's appId, so an open window can be raised instead of starting another instance.
	for (auto* window: qs::windows::WindowTracker::instance()->windows()) {
		if (window->appId().compare(aumid, Qt::CaseInsensitive) == 0) {
			window->activate();
			return;
		}
	}

	auto token = desktopEntry.isEmpty()
	               ? QString()
	               : qs::windows::WindowsDesktopEntryBackend::parsingNameForId(desktopEntry);

	// shell:AppsFolder\<AUMID> resolves registered AUMIDs even without a Start menu entry.
	qs::windows::WindowsDesktopEntryBackend::launch(token.isEmpty() ? aumid : token);
}

ToastMirror* NotificationServer::mirror() {
	if (this->mMirror) return this->mMirror;

	// Mirrored toasts are matched to their Start menu entries by AUMID; make sure the (async)
	// Apps folder scan has started before the first one can arrive.
	DesktopEntryManager::instance();

	this->mMirror = new ToastMirror(this);

	QObject::connect(this->mMirror, &ToastMirror::toastAdded, this, &NotificationServer::onToastAdded);
	QObject::connect(this->mMirror, &ToastMirror::toastRemoved, this, &NotificationServer::onToastRemoved);

	QObject::connect(
	    this->mMirror,
	    &ToastMirror::accessChanged,
	    this,
	    &NotificationServer::systemAccessChanged
	);

	QObject::connect(
	    this->mMirror,
	    &ToastMirror::activeChanged,
	    this,
	    &NotificationServer::systemMirrorActiveChanged
	);

	return this->mMirror;
}

void NotificationServer::setMirrorEnabled(bool enabled) {
	if (!enabled && !this->mMirror) return;
	this->mirror()->setEnabled(enabled);
}

void NotificationServer::requestSystemAccess() { this->mirror()->requestAccess(); }

SystemNotificationAccess::Enum NotificationServer::systemAccess() const {
	return this->mMirror ? this->mMirror->access() : SystemNotificationAccess::Unknown;
}

bool NotificationServer::isSystemMirrorActive() const {
	return this->mMirror && this->mMirror->isActive();
}

void NotificationServer::onToastAdded(const ToastSnapshot& toast) {
	if (this->toastMap.contains(toast.id)) return;

	auto hints = QVariantMap();
	hints.insert("x-windows-aumid", toast.aumid);
	hints.insert("x-windows-toast-id", toast.id);

	// Start menu entry of the sender, for its icon (when Windows has no logo for it) and for
	// launching it.
	auto desktopEntry = qs::windows::WindowsDesktopEntryBackend::idForParsingName(toast.aumid);
	if (!desktopEntry.isEmpty()) hints.insert("desktop-entry", desktopEntry);

	qCDebug(logNotifications) << "Mirroring toast" << toast.id << "from" << toast.aumid
	                          << "entry" << desktopEntry;

	this->deliver(
	    0,
	    0,
	    toast.appName,
	    toast.logoPath,
	    toast.summary,
	    // Toast text is plain text, while notification bodies are rendered as markup.
	    toast.body.toHtmlEscaped(),
	    {QStringLiteral("default"), QStringLiteral("Open")},
	    hints,
	    -1,
	    toast.id,
	    toast.aumid
	);
}

void NotificationServer::onToastRemoved(quint32 toastId) {
	if (auto* notification = this->toastMap.value(toastId)) {
		this->deleteNotification(notification, NotificationCloseReason::CloseRequested);
	}
}

} // namespace qs::windows::services::notifications
