#include "desktop_host.hpp"
#include <cwchar>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>
#include <qtimer.h>

#include "../core/logcat.hpp"
#include "appbar.hpp"
#include "util.hpp"

namespace qs::windows {

namespace {

QS_LOGGING_CATEGORY(logDesktop, "quickshell.windows.desktop", QtWarningMsg);

constexpr const wchar_t* LISTENER_CLASS = L"QuickshellDesktopListener";

// Undocumented, used by every wallpaper engine since Windows 8: Progman creates the WorkerW
// that draws the wallpaper behind the icons. Some Windows 10 builds only react to one of the
// two parameter sets.
constexpr UINT SPAWN_WORKERW = 0x052C;
constexpr UINT SPAWN_TIMEOUT_MS = 1000;

// Explorer rebuilds the desktop a moment after the display change or the new wallpaper.
constexpr int SETTLE_MS = 500;

// Windows 11 24H2 moved the WorkerW into Progman.
constexpr DWORD BUILD_24H2 = 26100;

DesktopHost* gHost = nullptr; // NOLINT

DWORD processOf(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid;
}

bool hasClass(HWND hwnd, const wchar_t* name) {
	wchar_t cls[32] {};
	if (GetClassNameW(hwnd, cls, 32) == 0) return false;
	return wcscmp(cls, name) == 0;
}

struct Lookup {
	HWND parent = nullptr;
	HWND insertAfter = nullptr;
	const char* layout = "";
};

// Windows 11 24H2 and later: Progman > { SHELLDLL_DefView, WorkerW }.
Lookup findChildWorkerW(HWND progman) {
	auto* workerw = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
	// A hidden parent would hide the panels with it.
	if (workerw == nullptr || !IsWindowVisible(workerw)) return {};
	return {.parent = workerw, .layout = "WorkerW inside Progman"};
}

// Windows 10 and 11 before 24H2: a top level WorkerW right behind the top level window that
// holds the icons view (Progman itself, or another WorkerW with a slideshow).
Lookup findTopLevelWorkerW(HWND progman) {
	struct Search {
		DWORD pid = 0;
		HWND iconsHost = nullptr;
	} search {.pid = processOf(progman)};

	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    auto* search = reinterpret_cast<Search*>(param); // NOLINT(performance-no-int-to-ptr)
		    if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) == nullptr) return TRUE;
		    if (processOf(hwnd) != search->pid) return TRUE;
		    search->iconsHost = hwnd;
		    return FALSE;
	    },
	    reinterpret_cast<LPARAM>(&search)
	);

	if (search.iconsHost == nullptr) return {};

	// the next WorkerW down the z order
	auto* workerw = FindWindowExW(nullptr, search.iconsHost, L"WorkerW", nullptr);
	if (workerw == nullptr || processOf(workerw) != search.pid || !IsWindowVisible(workerw)) {
		return {};
	}

	if (FindWindowExW(workerw, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr) return {};
	return {.parent = workerw, .layout = "WorkerW behind the icons"};
}

// No WorkerW: Progman draws the wallpaper itself, and its icons view is a child of it.
Lookup findProgman(HWND progman) {
	auto* icons = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
	if (icons == nullptr) return {};
	return {.parent = progman, .insertAfter = icons, .layout = "Progman, below the icons"};
}

Lookup findWorkerW(HWND progman) {
	// Both layouts are looked for on every build: insider and servicing builds moved between
	// them. The build only says which one is likelier.
	auto childLayout = windowsBuild() >= BUILD_24H2;
	auto found = childLayout ? findChildWorkerW(progman) : findTopLevelWorkerW(progman);
	if (found.parent == nullptr) {
		found = childLayout ? findTopLevelWorkerW(progman) : findChildWorkerW(progman);
	}

	return found;
}

} // namespace

DesktopHost* DesktopHost::instance() {
	static QPointer<DesktopHost> host; // NOLINT

	if (host.isNull()) {
		host = new DesktopHost(QCoreApplication::instance());
	}

	return host.data();
}

DesktopHost::DesktopHost(QObject* parent): QObject(parent) {
	gHost = this;
	this->refreshTimer.setSingleShot(true);
	QObject::connect(&this->refreshTimer, &QTimer::timeout, this, &DesktopHost::refresh);
}

DesktopHost::~DesktopHost() {
	this->removeHook();
	if (this->listener != nullptr) DestroyWindow(this->listener);
	if (gHost == this) gHost = nullptr;
}

void DesktopHost::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;
	emit this->enabledChanged();
	this->refresh();
}

HWND DesktopHost::parentWindow() {
	if (!this->mEnabled) return nullptr;

	// Explorer died since: the next refresh is likely queued already, but the caller is about
	// to use the handle now.
	if (!this->lookedUp || (this->mParent != nullptr && !IsWindow(this->mParent))) {
		this->refresh();
	}

	return this->mParent;
}

void DesktopHost::scheduleRefresh(int delayMs) { this->refreshTimer.start(delayMs); }

void DesktopHost::refresh() {
	this->refreshTimer.stop();

	auto* oldParent = this->mParent;
	auto* oldInsertAfter = this->mInsertAfter;
	auto wasActive = this->active();

	if (this->mEnabled) {
		this->ensureListener();
		this->lookup();
		this->installHook();
	} else {
		this->mParent = nullptr;
		this->mInsertAfter = nullptr;
		this->lookedUp = false;
		this->removeHook();
	}

	if (this->mParent != oldParent || this->mInsertAfter != oldInsertAfter) {
		emit this->parentChanged();
	} else if (this->mParent != nullptr) {
		// Same window, possibly another size (display change).
		emit this->parentMoved();
	}

	if (this->active() != wasActive) emit this->activeChanged();
}

void DesktopHost::lookup() {
	this->lookedUp = true;

	Lookup found;
	auto* progman = FindWindowW(L"Progman", nullptr);

	if (progman != nullptr) {
		found = findWorkerW(progman);

		if (found.parent == nullptr) {
			DWORD_PTR result = 0;
			auto flags = SMTO_NORMAL | SMTO_ABORTIFHUNG;
			SendMessageTimeoutW(progman, SPAWN_WORKERW, 0xD, 0x1, flags, SPAWN_TIMEOUT_MS, &result);
			found = findWorkerW(progman);

			if (found.parent == nullptr) {
				SendMessageTimeoutW(progman, SPAWN_WORKERW, 0, 0, flags, SPAWN_TIMEOUT_MS, &result);
				found = findWorkerW(progman);
			}
		}

		if (found.parent == nullptr) found = findProgman(progman);
	}

	if (found.parent != this->mParent) {
		if (found.parent != nullptr) {
			qCInfo(logDesktop) << "Desktop panels go into" << found.layout << "on build"
			                   << windowsBuild();
		} else {
			qCWarning(logDesktop) << "No desktop window found (explorer not running?);"
			                      << "desktop panels stay top level windows";
		}
	}

	this->mParent = found.parent;
	this->mInsertAfter = found.insertAfter;
}

void DesktopHost::ensureListener() {
	if (this->listener != nullptr) return;

	auto* module = GetModuleHandleW(nullptr);
	WNDCLASSW cls {};
	cls.lpfnWndProc = &DesktopHost::listenerProc;
	cls.hInstance = module;
	cls.lpszClassName = LISTENER_CLASS;
	RegisterClassW(&cls); // fails harmlessly if already registered

	// Hidden but top level: message-only windows get no broadcasts.
	this->listener = CreateWindowExW(
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
	    LISTENER_CLASS,
	    L"",
	    WS_POPUP,
	    0,
	    0,
	    0,
	    0,
	    nullptr,
	    nullptr,
	    module,
	    nullptr
	);

	if (this->listener == nullptr) {
		qCWarning(logDesktop) << "Failed to create the desktop listener window:" << GetLastError()
		                      << "- explorer restarts won't be followed";
		return;
	}

	// An elevated shell still hears a restarted (unelevated) explorer.
	ChangeWindowMessageFilterEx(
	    this->listener,
	    WinAppBar::taskbarCreatedMessage(),
	    MSGFLT_ALLOW,
	    nullptr
	);
}

LRESULT CALLBACK DesktopHost::listenerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	if (gHost != nullptr) {
		if (msg == WinAppBar::taskbarCreatedMessage()) {
			// Explorer restarted: its desktop windows are new ones.
			gHost->scheduleRefresh();
		} else if (msg == WM_DISPLAYCHANGE) {
			gHost->scheduleRefresh(SETTLE_MS);
		} else if (msg == WM_SETTINGCHANGE && wparam == SPI_SETDESKWALLPAPER) {
			gHost->scheduleRefresh(SETTLE_MS);
		}
	}

	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void DesktopHost::installHook() {
	DWORD pid = 0;
	auto thread = this->mParent == nullptr ? 0 : GetWindowThreadProcessId(this->mParent, &pid);
	if (thread == this->hookThread && (thread == 0 || this->hook != nullptr)) return;

	this->removeHook();
	if (thread == 0) return;

	// Out of context: the callback runs on this (the gui) thread, from its message loop.
	// Restricted to explorer's desktop thread, which also owns the parent's siblings.
	this->hook = SetWinEventHook(
	    EVENT_OBJECT_DESTROY,
	    EVENT_OBJECT_HIDE,
	    nullptr,
	    &DesktopHost::eventProc,
	    pid,
	    thread,
	    WINEVENT_OUTOFCONTEXT
	);

	this->moveHook = SetWinEventHook(
	    EVENT_OBJECT_LOCATIONCHANGE,
	    EVENT_OBJECT_LOCATIONCHANGE,
	    nullptr,
	    &DesktopHost::eventProc,
	    pid,
	    thread,
	    WINEVENT_OUTOFCONTEXT
	);

	this->hookThread = thread;

	if (this->hook == nullptr || this->moveHook == nullptr) {
		qCWarning(logDesktop) << "Could not watch the desktop window:" << GetLastError();
	}
}

void DesktopHost::removeHook() {
	if (this->hook != nullptr) UnhookWinEvent(this->hook);
	if (this->moveHook != nullptr) UnhookWinEvent(this->moveHook);
	this->hook = nullptr;
	this->moveHook = nullptr;
	this->hookThread = 0;
}

void CALLBACK DesktopHost::eventProc(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	auto* host = gHost;
	if (host == nullptr || hwnd == nullptr || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
		return;
	}

	if (hwnd == host->mParent) {
		switch (event) {
		case EVENT_OBJECT_LOCATIONCHANGE:
			// Comes in bursts while explorer resizes the desktop.
			if (!host->movePending) {
				host->movePending = true;

				QTimer::singleShot(0, host, [host] {
					host->movePending = false;
					emit host->parentMoved();
				});
			}
			break;
		// Gone or hidden: the panels inside are too. Look again once explorer is done.
		case EVENT_OBJECT_DESTROY:
		case EVENT_OBJECT_HIDE: host->scheduleRefresh(SETTLE_MS); break;
		default: break;
		}
	} else if (event == EVENT_OBJECT_SHOW && host->mInsertAfter != nullptr && hasClass(hwnd, L"WorkerW"))
	{
		// In Progman for lack of a WorkerW, and explorer just made one.
		host->scheduleRefresh(SETTLE_MS);
	}
}

// DesktopLayer

DesktopLayer::DesktopLayer(QObject* parent): QObject(parent) {
	auto* host = DesktopHost::instance();
	QObject::connect(host, &DesktopHost::enabledChanged, this, &DesktopLayer::enabledChanged);
	QObject::connect(host, &DesktopHost::activeChanged, this, &DesktopLayer::activeChanged);
}

bool DesktopLayer::enabled() const { return DesktopHost::instance()->enabled(); }
void DesktopLayer::setEnabled(bool enabled) { DesktopHost::instance()->setEnabled(enabled); }
bool DesktopLayer::active() const { return DesktopHost::instance()->active(); }

} // namespace qs::windows
