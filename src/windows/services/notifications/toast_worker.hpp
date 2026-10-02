#pragma once

#include <qhash.h>
#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include "toast_mirror.hpp"

class QTimer;

namespace qs::windows::services::notifications {

///! Runs on ToastMirror's MTA thread and owns the UserNotificationListener.
/// Created with no parent and moved to the thread before it starts; every method other than the
/// constructor runs on that thread. Commands arrive as queued calls from ToastMirror, results go
/// back the same way (`QMetaObject::invokeMethod(frontend, ..., Qt::QueuedConnection)`).
class ToastMirrorWorker: public QObject {
	Q_OBJECT;

public:
	explicit ToastMirrorWorker(ToastMirror* frontend);
	~ToastMirrorWorker() override;
	Q_DISABLE_COPY_MOVE(ToastMirrorWorker);

	void setEnabled(bool enabled);
	void requestAccess();
	void removeToast(quint32 id);

public slots:
	// Connected to QThread::started/finished: the apartment and the listener live exactly as
	// long as the thread.
	void start();
	void shutdown();

private:
	void begin(bool request);
	void stop();
	void applyAccess(winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus status);
	void checkAccess();
	void resync(bool announce);
	void report();

	[[nodiscard]] ToastSnapshot
	snapshotOf(const winrt::Windows::UI::Notifications::UserNotification& toast);
	QString logoFor(const winrt::Windows::ApplicationModel::AppInfo& info, const QString& aumid);

	ToastMirror* mFrontend;
	bool mApartment = false;
	bool mEnabled = false;

	winrt::Windows::UI::Notifications::Management::UserNotificationListener mListener {nullptr};
	winrt::event_token mChangedToken {};
	bool mEvents = false;

	QTimer* mPollTimer = nullptr;   // 2 s resync while polling
	QTimer* mAccessTimer = nullptr; // re-reads the access status while it isn't Allowed
	QTimer* mEventTimer = nullptr;  // coalesces bursts of NotificationChanged events

	bool mBaselined = false;
	QSet<quint32> mKnown;
	QHash<QString, QString> mLogos; // AUMID -> cached logo path ("" if Windows has none), this run

	SystemNotificationAccess::Enum mAccess = SystemNotificationAccess::Unknown;
	bool mActive = false;
};

} // namespace qs::windows::services::notifications
