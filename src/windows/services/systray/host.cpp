#include "host.hpp"
#include <algorithm>
#include <thread>
#include <utility>
#include <vector>

#include <qt_windows.h>
#include <shellapi.h>

#include <qcoreapplication.h>
#include <qfileinfo.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qstring.h>
#include <qstringbuilder.h>
#include <qtenvironmentvariables.h>
#include <quuid.h>

#include "../../../core/logcat.hpp"
#include "hook.hpp"
#include "item.hpp"

namespace qs::windows::services::systray {

namespace {

QS_LOGGING_CATEGORY(logTrayHost, "quickshell.windows.systray", QtWarningMsg);

// Owners that went away without NIM_DELETE (crashed apps) are found by polling: their windows
// are hidden or message-only, so no window event announces it.
constexpr int PRUNE_INTERVAL_MS = 2000;
constexpr ULONGLONG DELETION_MEMORY_MS = 10000;

// After explorer's window was in front: lets explorer settle and the apps' traffic of that
// moment reach it.
constexpr int RESYNC_DELAY_MS = 300;
// Explorer comes to the front in bursts (taskbar clicks, auto-hide); one read covers them.
constexpr ULONGLONG SNAPSHOT_MIN_GAP_MS = 2000;
// An icon missing from explorer's list is only dropped when the next read agrees.
constexpr int CONFIRM_DELAY_MS = 2000;
// Apps answer the TaskbarCreated broadcast in this time; what's still without a callback
// message after it is looked for elsewhere.
constexpr int RECOVER_DELAY_MS = 1500;
// An app that doesn't answer TaskbarCreated isn't asked again and again.
constexpr ULONGLONG ANNOUNCE_COOLDOWN_MS = 15000;
// ...unless the icon is clicked and this long went by since.
constexpr ULONGLONG CLICK_ANNOUNCE_GAP_MS = 2000;
// Brief mode: how often explorer's list is read, which is how fast icon changes show.
constexpr int BRIEF_POLL_MS = 1500;

struct KnownIcon {
	QUuid guid;
	const char* id;
	const char* title;
	Category::Enum category;
	// ii has its own volume, network and battery indicators.
	bool skip;
};

// Explorer's own icons, by the GUIDs its system tray object registers them with.
const KnownIcon* knownIcon(const QUuid& guid) {
	// NOLINTBEGIN(cert-err58-cpp)
	static const KnownIcon icons[] = {
	    {QUuid("{7820ae73-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Volume", "Volume", Category::Hardware, true},
	    {QUuid("{7820ae74-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Network", "Network", Category::Hardware, true},
	    {QUuid("{7820ae75-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Power", "Power", Category::Hardware, true},
	    {QUuid("{7820ae76-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Health", "Security and Maintenance", Category::SystemServices, false},
	    {QUuid("{7820ae77-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Location", "Location", Category::SystemServices, false},
	    {QUuid("{7820ae78-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Hardware", "Safely Remove Hardware", Category::Hardware, false},
	    {QUuid("{7820ae81-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Update", "Windows Update", Category::SystemServices, false},
	    {QUuid("{7820ae82-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Microphone", "Microphone", Category::Hardware, false},
	    {QUuid("{7820ae83-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.MeetNow", "Meet Now", Category::Communications, false},
	};
	// NOLINTEND(cert-err58-cpp)

	if (guid.isNull()) return nullptr;

	for (const auto& icon: icons) {
		if (icon.guid == guid) return &icon;
	}

	return nullptr;
}

QString exePathOf(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0) return {};

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process == nullptr) return {};

	wchar_t path[MAX_PATH] {};
	DWORD size = MAX_PATH;
	auto ok = QueryFullProcessImageNameW(process, 0, path, &size);
	CloseHandle(process);

	return ok ? QString::fromWCharArray(path, static_cast<qsizetype>(size)) : QString();
}

// The FileDescription string of an executable, what Task Manager shows as its name.
QString fileDescription(const QString& path) {
	if (path.isEmpty()) return {};

	auto native = path.toStdWString();
	DWORD unused = 0;
	auto size = GetFileVersionInfoSizeW(native.c_str(), &unused);
	if (size == 0) return {};

	std::vector<char> data(size);
	if (!GetFileVersionInfoW(native.c_str(), 0, size, data.data())) return {};

	struct Translation {
		WORD language;
		WORD codePage;
	};

	Translation* translation = nullptr;
	UINT length = 0;
	if (!VerQueryValueW(
	        data.data(),
	        L"\\VarFileInfo\\Translation",
	        reinterpret_cast<void**>(&translation), // NOLINT
	        &length
	    )
	    || length < sizeof(Translation))
	{
		return {};
	}

	auto key = QStringLiteral("\\StringFileInfo\\%1%2\\FileDescription")
	               .arg(translation->language, 4, 16, QChar(u'0'))
	               .arg(translation->codePage, 4, 16, QChar(u'0'))
	               .toStdWString();

	wchar_t* value = nullptr;
	if (!VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &length) // NOLINT
	    || value == nullptr || length == 0)
	{
		return {};
	}

	return QString::fromWCharArray(value).trimmed();
}

} // namespace

TrayHost* TrayHost::instance() {
	// Never deleted: the hook thread may still post to it while the process exits.
	static auto* host = new TrayHost(); // NOLINT
	return host;
}

TrayHost::TrayHost() {
	this->pruneTimer.setInterval(PRUNE_INTERVAL_MS);
	QObject::connect(&this->pruneTimer, &QTimer::timeout, this, &TrayHost::pruneDead);
	this->pruneTimer.start();

	this->snapshotTimer.setSingleShot(true);
	QObject::connect(&this->snapshotTimer, &QTimer::timeout, this, &TrayHost::startSnapshot);

	this->recoverTimer.setSingleShot(true);
	this->recoverTimer.setInterval(RECOVER_DELAY_MS);
	QObject::connect(&this->recoverTimer, &QTimer::timeout, this, &TrayHost::recoverCallbacks);

	this->sink = [this](TrayIconMessage message) {
		QMetaObject::invokeMethod(
		    this,
		    [this, message = std::move(message)]() { this->apply(message); },
		    Qt::QueuedConnection
		);
	};

	auto missedTraffic = [this]() {
		QMetaObject::invokeMethod(
		    this,
		    [this]() { this->scheduleSnapshot(RESYNC_DELAY_MS); },
		    Qt::QueuedConnection
		);
	};

	auto mode = qEnvironmentVariable("QS_WINDOWS_TRAY_HOOK");

	// Escape hatch for tools that need FindWindow(L"Shell_TrayWnd") to be explorer's. Without the
	// hook, icons we seed now would never update or go away (nothing is watching for that), so
	// skip seeding too: the tray stays empty instead of filling with icons stuck at startup state.
	if (mode == QStringLiteral("0")) {
		qCInfo(logTrayHost) << "QS_WINDOWS_TRAY_HOOK=0: not hooking the system tray";
		return;
	}

	// The middle way for such tools: the hook only steps in front of explorer's tray for a few
	// seconds when apps are asked to add their icons again, and icon changes come from reading
	// explorer's list every BRIEF_POLL_MS (so they show that much later, and an icon's hidden
	// state isn't followed).
	auto brief = mode == QStringLiteral("brief");
	if (brief) {
		qCInfo(logTrayHost) << "QS_WINDOWS_TRAY_HOOK=brief: the tray hook stays behind explorer's"
		                    << "tray but for short moments";

		this->pollTimer.setInterval(BRIEF_POLL_MS);
		QObject::connect(&this->pollTimer, &QTimer::timeout, this, [this]() {
			if (!this->snapshotRunning) this->startSnapshot();
		});
		this->pollTimer.start();
	}

	TrayHook::start(this->sink, missedTraffic, brief);

	QObject::connect(
	    QCoreApplication::instance(),
	    &QCoreApplication::aboutToQuit,
	    this,
	    []() { TrayHook::stop(); }
	);

	this->startSnapshot();
}

void TrayHost::apply(const TrayIconMessage& message) {
	if (message.hwnd == nullptr && message.guid.isNull()) return;

	auto* item = this->find(message.hwnd, message.uid, message.guid);

	switch (message.message) {
	case NIM_ADD:
	case NIM_MODIFY:
		if (message.seeded) this->snapshotReported++;

		if (item == nullptr) {
			if (!message.seeded || !this->deletedRecently(message)) this->add(message);
		} else if (!message.seeded) {
			item->update(message);
			item->tracking.hookUpdatedAt = GetTickCount64();
		} else {
			item->tracking.seenByExplorer = true;
			item->tracking.seenThisSnapshot = true;
			item->tracking.missedSnapshots = 0;

			// Whatever the hook saw since this read began is newer than explorer's list.
			if (item->tracking.hookUpdatedAt < this->snapshotStartedAt) {
				item->refreshFromExplorer(message);
			}
		}
		break;
	case NIM_SETVERSION:
		if (item != nullptr) {
			item->update(message);
			item->tracking.hookUpdatedAt = GetTickCount64();
		}
		break;
	case NIM_DELETE:
		if (item != nullptr) this->remove(item);
		this->deletions.append({message.hwnd, message.uid, message.guid, GetTickCount64()});
		break;
	default: break;
	}
}

void TrayHost::add(const TrayIconMessage& message) {
	// A new icon needs a live owner to talk to.
	if (message.hwnd == nullptr || !IsWindow(message.hwnd)) return;

	const auto* known = knownIcon(message.guid);
	if (known != nullptr && known->skip) return;

	auto exePath = message.exePath.isEmpty() ? exePathOf(message.hwnd) : message.exePath;
	auto exeName = QFileInfo(exePath).completeBaseName();

	auto id = known != nullptr ? QString::fromLatin1(known->id) : exeName;
	if (id.isEmpty()) id = QStringLiteral("unknown");

	auto title = known != nullptr ? QString::fromLatin1(known->title) : fileDescription(exePath);
	if (title.isEmpty()) title = exeName;

	auto* item = new SystemTrayItem(message.hwnd, message.uid, message.guid, this);
	if (known != nullptr) item->setCategory(known->category);
	item->update(message);
	item->setIdentity(this->uniqueId(id, message.uid), title, exePath);

	if (message.seeded) {
		item->tracking.seenByExplorer = true;
		item->tracking.seenThisSnapshot = true;
	} else {
		item->tracking.hookUpdatedAt = GetTickCount64();
	}

	item->onCallbackNeeded = [this, item]() {
		auto now = GetTickCount64();
		if (now - item->tracking.announcedAt >= CLICK_ANNOUNCE_GAP_MS) item->tracking.announcedAt = 0;
		this->recoverCallbacks();
	};

	qCDebug(logTrayHost) << "Added tray icon" << item->bindableId().value()
	                     << (message.seeded ? "from explorer's list" : "")
	                     << (item->hasCallback() ? "" : "(no callback message yet)");

	this->mItems.append(item);
	emit this->itemAdded(item);

	// Restarted by every such icon, so a burst (the seed) is handled once.
	if (!item->hasCallback()) this->recoverTimer.start();
}

void TrayHost::remove(SystemTrayItem* item) {
	qCDebug(logTrayHost) << "Removed tray icon" << item->bindableId().value();

	this->mItems.removeOne(item);
	emit this->itemRemoved(item);
	item->deleteLater();
}

void TrayHost::pruneDead() {
	auto items = this->mItems;
	for (auto* item: items) {
		if (!item->ownerAlive()) this->remove(item);
	}

	auto now = GetTickCount64();
	this->deletions.removeIf([&](const Deletion& deletion) {
		return now - deletion.at > DELETION_MEMORY_MS;
	});
}

void TrayHost::startSnapshot() {
	if (this->snapshotRunning) {
		this->snapshotAgain = true;
		return;
	}

	this->snapshotRunning = true;
	this->snapshotAgain = false;
	this->snapshotStartedAt = GetTickCount64();
	this->snapshotReported = 0;

	for (auto* item: this->mItems) {
		item->tracking.seenThisSnapshot = false;
	}

	// The icons come through the sink like the hook's, so done() is queued after all of them.
	seedFromExplorer(this->sink, [this](bool ok) {
		QMetaObject::invokeMethod(
		    this,
		    [this, ok]() { this->onSnapshotDone(ok); },
		    Qt::QueuedConnection
		);
	});
}

void TrayHost::scheduleSnapshot(int delayMs) {
	// Not pushed back by later requests, or a busy taskbar would postpone it forever.
	if (this->snapshotTimer.isActive()) return;

	auto sinceLast = GetTickCount64() - this->snapshotDoneAt;
	auto gap = sinceLast >= SNAPSHOT_MIN_GAP_MS ? 0 : SNAPSHOT_MIN_GAP_MS - sinceLast;

	this->snapshotTimer.start(std::max(delayMs, static_cast<int>(gap)));
}

void TrayHost::onSnapshotDone(bool ok) {
	this->snapshotRunning = false;
	this->snapshotDoneAt = GetTickCount64();

	auto confirm = false;

	// An empty list is what a just restarted explorer has: nothing to conclude from it.
	if (ok && this->snapshotReported > 0) {
		auto items = this->mItems;
		for (auto* item: items) {
			auto& tracking = item->tracking;

			// Only icons explorer listed before can go missing from its list: some of its own
			// may never be in there, and hidden icons may not be either.
			if (tracking.seenThisSnapshot || !tracking.seenByExplorer
			    || tracking.hookUpdatedAt >= this->snapshotStartedAt
			    || item->bindableStatus().value() == Status::Passive)
			{
				continue;
			}

			// Deleted while explorer's window was in front of ours.
			if (++tracking.missedSnapshots >= 2) {
				this->deletions.append(
				    {item->ownerWindow(), item->iconId(), item->iconGuid(), GetTickCount64()}
				);
				this->remove(item);
			} else {
				confirm = true;
			}
		}
	}

	if (confirm || this->snapshotAgain) this->scheduleSnapshot(CONFIRM_DELAY_MS);
}

void TrayHost::recoverCallbacks() {
	auto missing = std::ranges::any_of(this->mItems, [](SystemTrayItem* item) {
		return !item->hasCallback();
	});

	if (!missing) return;

	if (this->toolbarReading) {
		this->recoverAgain = true;
		return;
	}

	this->toolbarReading = true;

	// Cross process calls into explorer: off the GUI thread.
	std::thread([this]() {
		SetThreadDescription(GetCurrentThread(), L"qs tray toolbars");
		auto icons = readExplorerToolbars();

		QMetaObject::invokeMethod(
		    this,
		    [this, icons = std::move(icons)]() { this->onToolbarData(icons); },
		    Qt::QueuedConnection
		);
	}).detach();
}

void TrayHost::onToolbarData(const std::vector<ExplorerIconData>& icons) {
	this->toolbarReading = false;

	for (const auto& icon: icons) {
		auto* item = this->find(icon.hwnd, icon.uid, QUuid());
		if (item == nullptr || item->hasCallback()) continue;

		qCDebug(logTrayHost) << "Callback message of" << item->bindableId().value()
		                     << "from explorer's notification area";
		item->applyExplorerData(icon.callbackMessage, icon.version);
	}

	// The rest: ask their apps to add them again, which goes through the hook. Explorer keeps
	// them where they are (the hook turns the add into an update).
	auto now = GetTickCount64();
	std::vector<HWND> owners;

	for (auto* item: this->mItems) {
		if (item->hasCallback() || now - item->tracking.announcedAt < ANNOUNCE_COOLDOWN_MS) continue;
		item->tracking.announcedAt = now;
		owners.push_back(item->ownerWindow());
	}

	if (!owners.empty()) {
		qCDebug(logTrayHost) << "Asking" << owners.size()
		                     << "tray icon owners to add their icons again";
		TrayHook::announceTo(std::move(owners));
	}

	if (this->recoverAgain) {
		this->recoverAgain = false;
		this->recoverCallbacks();
	}
}

SystemTrayItem* TrayHost::find(HWND hwnd, UINT uid, const QUuid& guid) const {
	for (auto* item: this->mItems) {
		if (item->matches(hwnd, uid, guid)) return item;
	}

	return nullptr;
}

// Ids are what ii's pin list stores, so they come from the executable (stable across runs)
// rather than from window handles.
QString TrayHost::uniqueId(const QString& base, UINT uid) const {
	auto taken = [this](const QString& id) {
		for (auto* item: this->mItems) {
			if (item->bindableId().value() == id) return true;
		}
		return false;
	};

	if (!taken(base)) return base;

	QString id = base % u':' % QString::number(uid);
	for (auto n = 2; taken(id); n++) {
		id = base % u':' % QString::number(uid) % u'-' % QString::number(n);
	}

	return id;
}

bool TrayHost::deletedRecently(const TrayIconMessage& message) {
	auto now = GetTickCount64();

	for (const auto& deletion: this->deletions) {
		if (now - deletion.at > DELETION_MEMORY_MS) continue;
		if (!message.guid.isNull() && message.guid == deletion.guid) return true;
		if (message.hwnd == deletion.hwnd && message.uid == deletion.uid) return true;
	}

	return false;
}

} // namespace qs::windows::services::systray
