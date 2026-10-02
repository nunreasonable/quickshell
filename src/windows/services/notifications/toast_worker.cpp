#include "toast_worker.hpp"

#include <winerror.h>

#include <qbytearray.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qimage.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qstandardpaths.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/base.h>

#include "../../../core/logcat.hpp"
#include "toast_mirror.hpp"

using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;
using winrt::Windows::ApplicationModel::AppInfo;

namespace qs::windows::services::notifications {

namespace {
QS_LOGGING_CATEGORY(logToastMirror, "quickshell.windows.notifications.mirror", QtWarningMsg);

constexpr int POLL_INTERVAL_MS = 2000;
constexpr int ACCESS_CHECK_INTERVAL_MS = 10000;
constexpr int EVENT_COALESCE_MS = 150;
constexpr float LOGO_SIZE = 96;
constexpr uint32_t MAX_LOGO_BYTES = 4 * 1024 * 1024;

QString toQString(const winrt::hstring& value) {
	return QString::fromWCharArray(value.c_str(), static_cast<qsizetype>(value.size()));
}

uint32_t codeOf(const winrt::hresult_error& e) { return static_cast<uint32_t>(e.code().value); }

SystemNotificationAccess::Enum mapAccess(UserNotificationListenerAccessStatus status) {
	switch (status) {
	case UserNotificationListenerAccessStatus::Allowed: return SystemNotificationAccess::Allowed;
	case UserNotificationListenerAccessStatus::Denied: return SystemNotificationAccess::Denied;
	default: return SystemNotificationAccess::Unspecified;
	}
}

QString logoCacheDir() {
	auto dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
	         + QStringLiteral("/notification-icons");
	QDir().mkpath(dir);
	return dir;
}
} // namespace

ToastMirrorWorker::ToastMirrorWorker(ToastMirror* frontend): mFrontend(frontend) {}

ToastMirrorWorker::~ToastMirrorWorker() = default;

void ToastMirrorWorker::start() {
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		this->mApartment = true;
	} catch (const winrt::hresult_error& e) {
		qCWarning(logToastMirror) << "init_apartment(multi_threaded) failed:" << Qt::hex << codeOf(e);
	}

	// Created here so they belong to this thread.
	this->mPollTimer = new QTimer(this);
	this->mPollTimer->setInterval(POLL_INTERVAL_MS);
	QObject::connect(this->mPollTimer, &QTimer::timeout, this, [this] { this->resync(true); });

	this->mAccessTimer = new QTimer(this);
	this->mAccessTimer->setInterval(ACCESS_CHECK_INTERVAL_MS);
	QObject::connect(this->mAccessTimer, &QTimer::timeout, this, &ToastMirrorWorker::checkAccess);

	this->mEventTimer = new QTimer(this);
	this->mEventTimer->setSingleShot(true);
	this->mEventTimer->setInterval(EVENT_COALESCE_MS);
	QObject::connect(this->mEventTimer, &QTimer::timeout, this, [this] { this->resync(true); });
}

void ToastMirrorWorker::shutdown() {
	this->mEnabled = false;
	this->stop();
	this->mListener = nullptr;

	if (this->mApartment) {
		winrt::uninit_apartment();
		this->mApartment = false;
	}
}

void ToastMirrorWorker::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;

	if (enabled) this->begin(true);
	else {
		this->stop();
		this->report();
	}
}

void ToastMirrorWorker::requestAccess() { this->begin(true); }

void ToastMirrorWorker::begin(bool request) {
	if (!this->mApartment) {
		this->mAccess = SystemNotificationAccess::Unavailable;
		this->report();
		return;
	}

	try {
		if (!this->mListener) this->mListener = UserNotificationListener::Current();

		auto status = this->mListener.GetAccessStatus();

		// Only Unspecified is worth asking about: a Denied comes from the notification access
		// switch in Settings, which only the user can flip back. With that switch on (the
		// Windows default) the status is already Allowed for an unpackaged exe, no prompt.
		if (request && status == UserNotificationListenerAccessStatus::Unspecified) {
			status = this->mListener.RequestAccessAsync().get();
		}

		this->applyAccess(status);
	} catch (const winrt::hresult_error& e) {
		qCWarning(logToastMirror) << "UserNotificationListener unavailable:" << Qt::hex << codeOf(e)
		                          << QString::fromWCharArray(e.message().c_str());
		this->stop();
		this->mAccess = SystemNotificationAccess::Unavailable;
		this->report();
	}
}

void ToastMirrorWorker::applyAccess(UserNotificationListenerAccessStatus status) {
	this->mAccess = mapAccess(status);

	if (this->mAccess != SystemNotificationAccess::Allowed || !this->mEnabled) {
		this->stop();
		// Keep looking: the user may turn notification access on in Settings at any time.
		if (this->mEnabled) this->mAccessTimer->start();
		this->report();
		return;
	}

	this->mAccessTimer->stop();

	if (!this->mActive) {
		if (!this->mEvents) {
			try {
				this->mChangedToken = this->mListener.NotificationChanged([this](auto&&, auto&&) {
					// Arbitrary thread pool thread: hop to ours before touching anything.
					QMetaObject::invokeMethod(
					    this,
					    [this] {
						    if (this->mActive) this->mEventTimer->start();
					    },
					    Qt::QueuedConnection
					);
				});
				this->mEvents = true;
			} catch (const winrt::hresult_error& e) {
				// 0x80070490 (ERROR_NOT_FOUND) without package identity, which is how qs runs.
				qCInfo(logToastMirror) << "NotificationChanged unavailable (" << Qt::hex << codeOf(e)
				                       << "), polling every" << POLL_INTERVAL_MS << "ms";
			}
		}

		this->mActive = true;
		this->mKnown.clear();
		this->resync(false); // baseline: what's already in the notification center isn't new

		if (this->mActive && !this->mEvents) this->mPollTimer->start();
	}

	this->report();
}

void ToastMirrorWorker::checkAccess() {
	if (!this->mListener || !this->mEnabled) return;

	try {
		auto status = this->mListener.GetAccessStatus();
		if (mapAccess(status) != this->mAccess || status == UserNotificationListenerAccessStatus::Allowed) {
			this->applyAccess(status);
		}
	} catch (const winrt::hresult_error& e) {
		qCWarning(logToastMirror) << "GetAccessStatus failed:" << Qt::hex << codeOf(e);
	}
}

void ToastMirrorWorker::stop() {
	if (this->mPollTimer) this->mPollTimer->stop();
	if (this->mEventTimer) this->mEventTimer->stop();
	if (this->mAccessTimer) this->mAccessTimer->stop();

	if (this->mEvents) {
		try {
			this->mListener.NotificationChanged(this->mChangedToken);
		} catch (const winrt::hresult_error&) {}
		this->mEvents = false;
	}

	this->mKnown.clear();
	this->mActive = false;
}

void ToastMirrorWorker::resync(bool announce) {
	if (!this->mActive) return;

	winrt::Windows::Foundation::Collections::IVectorView<UserNotification> toasts {nullptr};
	try {
		toasts = this->mListener.GetNotificationsAsync(NotificationKinds::Toast).get();
	} catch (const winrt::hresult_error& e) {
		// Most likely notification access was turned off in Settings while mirroring.
		qCWarning(logToastMirror) << "GetNotificationsAsync failed:" << Qt::hex << codeOf(e);
		this->stop();
		this->mAccess = e.code() == E_ACCESSDENIED ? SystemNotificationAccess::Denied
		                                           : SystemNotificationAccess::Unspecified;
		if (this->mEnabled) this->mAccessTimer->start();
		this->report();
		return;
	}

	auto current = QSet<quint32>();

	for (const auto& toast: toasts) {
		quint32 id = 0;
		try {
			id = toast.Id();
		} catch (const winrt::hresult_error&) {
			continue;
		}

		current.insert(id);
		if (!announce || this->mKnown.contains(id)) continue;

		auto snapshot = this->snapshotOf(toast);
		qCDebug(logToastMirror) << "New toast" << id << "from" << snapshot.aumid;

		QMetaObject::invokeMethod(
		    this->mFrontend,
		    [frontend = this->mFrontend, snapshot] { emit frontend->toastAdded(snapshot); },
		    Qt::QueuedConnection
		);
	}

	for (auto id: std::as_const(this->mKnown)) {
		if (current.contains(id)) continue;
		qCDebug(logToastMirror) << "Toast" << id << "left the notification center";

		QMetaObject::invokeMethod(
		    this->mFrontend,
		    [frontend = this->mFrontend, id] { emit frontend->toastRemoved(id); },
		    Qt::QueuedConnection
		);
	}

	this->mKnown = current;
}

void ToastMirrorWorker::removeToast(quint32 id) {
	// Forget it first so the next resync doesn't report our own removal back.
	this->mKnown.remove(id);
	if (!this->mListener || !this->mActive) return;

	try {
		this->mListener.RemoveNotification(id);
	} catch (const winrt::hresult_error& e) {
		qCWarning(logToastMirror) << "RemoveNotification" << id << "failed:" << Qt::hex << codeOf(e);
	}
}

ToastSnapshot ToastMirrorWorker::snapshotOf(const UserNotification& toast) {
	auto snapshot = ToastSnapshot();
	snapshot.id = toast.Id();

	try {
		if (auto info = toast.AppInfo()) {
			snapshot.aumid = toQString(info.AppUserModelId());
			if (auto display = info.DisplayInfo()) snapshot.appName = toQString(display.DisplayName());
			snapshot.logoPath = this->logoFor(info, snapshot.aumid);
		}
	} catch (const winrt::hresult_error& e) {
		qCDebug(logToastMirror) << "AppInfo of toast" << snapshot.id << "failed:" << Qt::hex << codeOf(e);
	}

	if (snapshot.appName.isEmpty()) snapshot.appName = snapshot.aumid;
	if (snapshot.appName.isEmpty()) snapshot.appName = QStringLiteral("Windows");

	try {
		auto notification = toast.Notification();
		auto visual = notification ? notification.Visual() : nullptr;

		if (visual) {
			// Toasts are ToastGeneric nowadays (legacy templates are converted, keeping the old
			// name in a hint); fall back to whatever binding there is.
			auto binding = visual.GetBinding(KnownNotificationBindings::ToastGeneric());
			if (!binding && visual.Bindings().Size() != 0) binding = visual.Bindings().GetAt(0);

			if (binding) {
				auto lines = QStringList();
				for (const auto& text: binding.GetTextElements()) lines << toQString(text.Text());
				if (!lines.isEmpty()) snapshot.summary = lines.takeFirst();
				snapshot.body = lines.join('\n');
			}
		}
	} catch (const winrt::hresult_error& e) {
		qCDebug(logToastMirror) << "Reading toast" << snapshot.id << "failed:" << Qt::hex << codeOf(e);
	}

	return snapshot;
}

QString ToastMirrorWorker::logoFor(const AppInfo& info, const QString& aumid) {
	// Fetched once per AUMID per run (and rewritten then, so app updates are picked up).
	if (auto it = this->mLogos.constFind(aumid); it != this->mLogos.constEnd()) return *it;

	QString path;

	try {
		// Null for unpackaged Win32 senders (PowerShell, most classic apps); the Start menu
		// entry's shell icon covers those.
		auto logo = info.DisplayInfo().GetLogo(winrt::Windows::Foundation::Size(LOGO_SIZE, LOGO_SIZE));
		auto stream = logo ? logo.OpenReadAsync().get() : nullptr;

		if (stream && stream.Size() > 0 && stream.Size() <= MAX_LOGO_BYTES) {
			auto size = static_cast<uint32_t>(stream.Size());
			auto reader = winrt::Windows::Storage::Streams::DataReader(stream);
			reader.LoadAsync(size).get();

			auto bytes = QByteArray(static_cast<qsizetype>(size), Qt::Uninitialized);
			reader.ReadBytes(winrt::array_view<uint8_t>(
			    reinterpret_cast<uint8_t*>(bytes.data()),               // NOLINT
			    reinterpret_cast<uint8_t*>(bytes.data()) + bytes.size() // NOLINT
			));

			auto image = QImage();
			if (image.loadFromData(bytes)) {
				auto hash = QCryptographicHash::hash(aumid.toUtf8(), QCryptographicHash::Sha1).toHex();
				auto file = logoCacheDir() + '/' + QString::fromLatin1(hash) + QStringLiteral(".png");
				if (image.save(file, "PNG")) path = file;
				else qCWarning(logToastMirror) << "Could not save the logo of" << aumid << "to" << file;
			}
		}
	} catch (const winrt::hresult_error& e) {
		qCDebug(logToastMirror) << "GetLogo for" << aumid << "failed:" << Qt::hex << codeOf(e);
	}

	this->mLogos.insert(aumid, path);
	return path;
}

void ToastMirrorWorker::report() {
	QMetaObject::invokeMethod(
	    this->mFrontend,
	    [frontend = this->mFrontend, access = this->mAccess, active = this->mActive] {
		    frontend->workerState(access, active);
	    },
	    Qt::QueuedConnection
	);
}

} // namespace qs::windows::services::notifications
