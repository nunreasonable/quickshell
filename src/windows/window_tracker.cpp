#include "window_tracker.hpp"
#include <string>

#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpoint.h>
#include <qproperty.h>
#include <qrect.h>
#include <qscreen.h>
#include <qset.h>
#include <qstring.h>
#include <qtimer.h>
#include <qtypes.h>

#include "util.hpp"
#include "virtual_desktops.hpp"

// last: pulls in the rpc headers, which define macros like `small`.
// initguid.h first so PKEY_AppUserModel_ID gets defined in this translation unit.
#include <initguid.h>

#include <appmodel.h>
#include <dwmapi.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>

#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logTracker, "quickshell.windows.tracker", QtWarningMsg);

constexpr const wchar_t* UWP_FRAME_CLASS = L"ApplicationFrameWindow";
constexpr const wchar_t* UWP_CORE_CLASS = L"Windows.UI.Core.CoreWindow";

QString windowClass(HWND hwnd) {
	wchar_t buffer[128] {};
	auto length = GetClassNameW(hwnd, buffer, 128);
	return QString::fromWCharArray(buffer, length);
}

// Shell owned windows that pass the style checks but are never applications.
bool isShellClass(const QString& cls) {
	static const auto classes = QSet<QString> {
	    QStringLiteral("Windows.UI.Core.CoreWindow"), // hosted inside ApplicationFrameWindow
	    QStringLiteral("Progman"),
	    QStringLiteral("WorkerW"),
	    QStringLiteral("Shell_TrayWnd"),
	    QStringLiteral("Shell_SecondaryTrayWnd"),
	    QStringLiteral("Windows.Internal.Shell.TabProxyWindow"),
	    QStringLiteral("XamlExplorerHostIslandWindow"),
	    QStringLiteral("TopLevelWindowForOverflowXamlIsland"),
	};

	return classes.contains(cls);
}

// Explicit AppUserModelID of the window (UWP frames and apps that set one).
QString appUserModelIdOf(HWND hwnd) {
	IPropertyStore* store = nullptr;
	if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || store == nullptr) {
		return {};
	}

	QString id;
	PROPVARIANT value;
	PropVariantInit(&value);

	if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR) {
		id = QString::fromWCharArray(value.pwszVal);
	}

	PropVariantClear(&value);
	store->Release();
	return id;
}

// AppUserModelID of a packaged (MSIX / Store) process, e.g. Windows Terminal.
QString appUserModelIdOf(HANDLE process) {
	UINT32 length = 0;
	if (GetApplicationUserModelId(process, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER) return {};
	if (length == 0) return {};

	auto buffer = std::wstring(length, L'\0');
	if (GetApplicationUserModelId(process, &length, buffer.data()) != ERROR_SUCCESS) return {};
	return QString::fromWCharArray(buffer.data());
}

QString processImagePath(HANDLE process) {
	wchar_t buffer[1024] {};
	DWORD length = 1024;
	if (!QueryFullProcessImageNameW(process, 0, buffer, &length)) return {};
	return QString::fromWCharArray(buffer, length);
}

bool covers(const RECT& rect, const QRect& area) {
	return rect.left <= area.left() && rect.top <= area.top()
	    && rect.right >= area.left() + area.width() && rect.bottom >= area.top() + area.height();
}

} // namespace

// --- TrackedWindow -----------------------------------------------------------------------------

TrackedWindow::TrackedWindow(WindowTracker* tracker, HWND hwnd)
    : QObject(tracker)
    , tracker(tracker)
    , mHwnd(hwnd) {}

quint64 TrackedWindow::address() const {
	return static_cast<quint64>(reinterpret_cast<quintptr>(this->mHwnd));
}

QString TrackedWindow::addressHex() const { return QString::number(this->address(), 16); }

TrackedWindow* TrackedWindow::owner() const {
	auto* owner = GetWindow(this->mHwnd, GW_OWNER);
	return owner == nullptr ? nullptr : this->tracker->windowFor(owner);
}

void TrackedWindow::refreshIdentity() {
	DWORD pid = 0;
	GetWindowThreadProcessId(this->mHwnd, &pid);

	this->uwpFrame = windowClass(this->mHwnd) == QString::fromWCharArray(UWP_FRAME_CLASS);

	if (this->uwpFrame) {
		// ApplicationFrameHost.exe owns the frame; the app itself owns the hosted core window.
		auto* core = FindWindowExW(this->mHwnd, nullptr, UWP_CORE_CLASS, nullptr);
		if (core != nullptr) GetWindowThreadProcessId(core, &pid);
	}

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

	QString exe;
	auto appId = appUserModelIdOf(this->mHwnd);

	if (process != nullptr) {
		exe = processImagePath(process);
		if (appId.isEmpty()) appId = appUserModelIdOf(process);
		CloseHandle(process);
	}

	if (appId.isEmpty() && !exe.isEmpty()) {
		appId = QFileInfo(exe).fileName().toLower();
		if (appId.endsWith(".exe")) appId.chop(4);
	}

	Qt::beginPropertyUpdateGroup();
	this->bPid = pid;
	this->bExePath = exe;
	this->bAppId = appId;
	Qt::endPropertyUpdateGroup();
}

void TrackedWindow::refreshTitle() {
	// For another process' window this reads the cached caption, so it can't block on a hung app.
	auto length = GetWindowTextLengthW(this->mHwnd);
	QString title;

	if (length > 0) {
		auto buffer = std::wstring(static_cast<size_t>(length) + 1, L'\0');
		auto copied = GetWindowTextW(this->mHwnd, buffer.data(), length + 1);
		title = QString::fromWCharArray(buffer.data(), copied);
	}

	this->bTitle = title;
}

void TrackedWindow::refreshState() {
	auto minimized = IsIconic(this->mHwnd) != FALSE;
	auto maximized = IsZoomed(this->mHwnd) != FALSE;

	auto* monitor = MonitorFromWindow(this->mHwnd, MONITOR_DEFAULTTONEAREST);
	auto* screen = this->tracker->screenFor(monitor);
	auto rects = monitorRects(monitor);

	RECT raw {};
	GetWindowRect(this->mHwnd, &raw);

	// The frame bounds exclude the invisible resize borders, so they match what the user sees.
	RECT frame {};
	if (FAILED(DwmGetWindowAttribute(this->mHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
		frame = raw;
	}

	// A maximized window also covers the monitor when nothing reserves space, but that is not
	// fullscreen in the sense shells care about (hiding bars and wallpapers).
	auto fullscreen = !minimized && !maximized && rects.valid && covers(raw, rects.monitor);

	Qt::beginPropertyUpdateGroup();
	this->bMinimized = minimized;
	this->bMaximized = maximized;
	this->bFullscreen = fullscreen;
	this->bScreen = screen;

	if (screen != nullptr && rects.valid) {
		auto mapper = ScreenMapper(screen->geometry(), rects.monitor, screen->devicePixelRatio());

		if (!minimized) {
			this->bRect = mapper.toLogical(toQRect(frame));
		} else if (this->bRect.value().isNull()) {
			// Minimized windows sit at -32000; use where the window would be restored to.
			WINDOWPLACEMENT placement {};
			placement.length = sizeof(placement);
			if (GetWindowPlacement(this->mHwnd, &placement)) {
				this->bRect = mapper.toLogical(toQRect(placement.rcNormalPosition));
			}
		}
	}

	Qt::endPropertyUpdateGroup();
}

void TrackedWindow::refreshDesktop() {
	this->bDesktop = static_cast<qint32>(this->tracker->desktops()->windowDesktopIndex(this->mHwnd));
}

void TrackedWindow::activate() {
	auto* desktops = this->tracker->desktops();
	auto desktop = desktops->windowDesktopIndex(this->mHwnd);
	auto switched = false;

	if (desktop != -1 && desktop != desktops->currentIndex()) {
		switched = desktops->switchTo(desktop);
	}

	if (IsIconic(this->mHwnd)) ShowWindowAsync(this->mHwnd, SW_RESTORE);

	// Like the taskbar: a window blocked by its modal dialog gets the dialog focused instead.
	auto* target = GetLastActivePopup(this->mHwnd);
	if (target == nullptr || !IsWindowVisible(target)) target = this->mHwnd;

	auto raise = [target]() { forceForegroundWindow(target); };

	// Shortcut injected switches animate; the foreground request has to wait for them.
	if (switched && !desktops->accessorLoaded()) {
		QTimer::singleShot(400, this, raise);
	} else {
		raise();
	}
}

void TrackedWindow::close() { PostMessageW(this->mHwnd, WM_CLOSE, 0, 0); }

void TrackedWindow::setMinimized(bool minimized) {
	PostMessageW(this->mHwnd, WM_SYSCOMMAND, minimized ? SC_MINIMIZE : SC_RESTORE, 0);
}

void TrackedWindow::setMaximized(bool maximized) {
	PostMessageW(this->mHwnd, WM_SYSCOMMAND, maximized ? SC_MAXIMIZE : SC_RESTORE, 0);
}

void TrackedWindow::setFullscreen(bool fullscreen) {
	this->fullscreenOn(fullscreen ? this->bScreen.value() : nullptr);
}

void TrackedWindow::fullscreenOn(QScreen* screen) {
	auto& backup = this->fullscreenBackup;

	if (screen == nullptr) {
		if (!backup.active) {
			if (this->bFullscreen.value()) {
				qCInfo(logTracker) << "Window" << this->addressHex()
				                   << "made itself fullscreen; cannot undo that for it.";
			}
			return;
		}

		SetWindowLongPtrW(this->mHwnd, GWL_STYLE, backup.style);
		SetWindowPos(
		    this->mHwnd,
		    nullptr,
		    backup.rect.left,
		    backup.rect.top,
		    backup.rect.right - backup.rect.left,
		    backup.rect.bottom - backup.rect.top,
		    SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE
		);

		backup.active = false;
		return;
	}

	auto rects = monitorRects(monitorForScreen(screen));
	if (!rects.valid) return;

	if (!backup.active) {
		backup.style = GetWindowLongPtrW(this->mHwnd, GWL_STYLE);
		GetWindowRect(this->mHwnd, &backup.rect);
		backup.active = true;
	}

	if (IsZoomed(this->mHwnd)) ShowWindow(this->mHwnd, SW_RESTORE);

	SetWindowLongPtrW(this->mHwnd, GWL_STYLE, backup.style & ~(WS_CAPTION | WS_THICKFRAME));
	SetWindowPos(
	    this->mHwnd,
	    HWND_TOP,
	    rects.monitor.left(),
	    rects.monitor.top(),
	    rects.monitor.width(),
	    rects.monitor.height(),
	    SWP_FRAMECHANGED | SWP_NOOWNERZORDER
	);
}

bool TrackedWindow::moveToDesktop(qsizetype index) {
	if (!this->tracker->desktops()->moveWindow(this->mHwnd, index)) return false;
	this->refreshDesktop();
	return true;
}

void TrackedWindow::moveTo(const QPoint& logical) {
	auto* screen = QGuiApplication::screenAt(logical);
	if (screen == nullptr) screen = this->bScreen.value();
	if (screen == nullptr) return;

	auto rects = monitorRects(monitorForScreen(screen));
	if (!rects.valid) return;

	auto mapper = ScreenMapper(screen->geometry(), rects.monitor, screen->devicePixelRatio());
	auto physical = mapper.toPhysical(logical);

	// Position the visible frame, not the window rect that includes the invisible borders.
	RECT raw {};
	RECT frame {};
	GetWindowRect(this->mHwnd, &raw);
	if (FAILED(DwmGetWindowAttribute(this->mHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
		frame = raw;
	}

	SetWindowPos(
	    this->mHwnd,
	    nullptr,
	    physical.x() - (frame.left - raw.left),
	    physical.y() - (frame.top - raw.top),
	    0,
	    0,
	    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	);
}

// --- WindowTracker -----------------------------------------------------------------------------

WindowTracker* WindowTracker::instance() {
	static auto* instance = new WindowTracker(); // NOLINT
	return instance;
}

WindowTracker::WindowTracker() {
	this->mDesktops = VirtualDesktops::instance();

	this->flushTimer.setSingleShot(true);
	this->flushTimer.setInterval(0);
	QObject::connect(&this->flushTimer, &QTimer::timeout, this, &WindowTracker::flush);

	this->updateScreens();

	if (auto* app = qobject_cast<QGuiApplication*>(QGuiApplication::instance())) {
		QObject::connect(app, &QGuiApplication::screenAdded, this, &WindowTracker::onScreensChanged);
		QObject::connect(app, &QGuiApplication::screenRemoved, this, &WindowTracker::onScreensChanged);
	}

	QObject::connect(
	    this->mDesktops,
	    &VirtualDesktops::desktopsChanged,
	    this,
	    &WindowTracker::onDesktopsChanged
	);

	// Windows shown on every desktop follow the current one.
	QObject::connect(this->mDesktops, &VirtualDesktops::currentChanged, this, [this]() {
		this->foregroundDirty = true;
		this->schedule();
	});

	auto hook = [this](DWORD min, DWORD max) {
		auto* handle = SetWinEventHook(
		    min,
		    max,
		    nullptr,
		    &WindowTracker::eventProc,
		    0,
		    0,
		    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
		);

		if (handle == nullptr) {
			qCWarning(logTracker) << "SetWinEventHook failed for" << Qt::hex << min << max;
		} else {
			this->hooks.append(handle);
		}
	};

	// Out of context hooks deliver every event in the range through our message queue, so the
	// ranges stay tight. EVENT_OBJECT_CREATE is left out on purpose: nothing is known about a
	// window before it is shown, and creations are by far the most frequent events.
	hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
	hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
	hook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE);
	hook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE);
	hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);

	this->rescan();
}

WindowTracker::~WindowTracker() {
	for (auto* hook: this->hooks) UnhookWinEvent(hook);
}

void CALLBACK WindowTracker::eventProc(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD /*eventThread*/,
    DWORD /*eventTime*/
) {
	if (hwnd == nullptr || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
	WindowTracker::instance()->onEvent(event, hwnd);
}

void WindowTracker::onEvent(DWORD event, HWND hwnd) {
	auto tracked = this->byHwnd.contains(hwnd);

	switch (event) {
	case EVENT_SYSTEM_FOREGROUND:
		this->foregroundDirty = true;
		this->desktopsDirty = true;
		if (!tracked) this->candidates.insert(hwnd);
		break;
	case EVENT_OBJECT_LOCATIONCHANGE:
		if (!tracked) return;
		this->dirty[hwnd].state = true;
		break;
	case EVENT_OBJECT_DESTROY:
	case EVENT_OBJECT_HIDE:
		if (!tracked) return;
		this->dirty[hwnd].eligibility = true;
		break;
	case EVENT_OBJECT_SHOW:
		if (tracked) this->dirty[hwnd].eligibility = true;
		else this->candidates.insert(hwnd);
		break;
	case EVENT_OBJECT_NAMECHANGE:
		if (tracked) this->dirty[hwnd].title = true;
		else this->candidates.insert(hwnd);
		break;
	case EVENT_OBJECT_CLOAKED:
	case EVENT_OBJECT_UNCLOAKED:
		// Desktop switches cloak and uncloak; so do moves between desktops and UWP apps
		// suspending. Only the desktop query can tell them apart.
		this->desktopsDirty = true;
		this->foregroundDirty = true;
		if (tracked) {
			auto& d = this->dirty[hwnd];
			d.eligibility = true;
			d.desktop = true;
			d.state = true;
		} else {
			this->candidates.insert(hwnd);
		}
		break;
	case EVENT_SYSTEM_MINIMIZESTART:
	case EVENT_SYSTEM_MINIMIZEEND:
		if (tracked) this->dirty[hwnd].state = true;
		else this->candidates.insert(hwnd);
		break;
	default: return;
	}

	this->schedule();
}

void WindowTracker::schedule() {
	if (!this->flushTimer.isActive()) this->flushTimer.start();
}

void WindowTracker::flush() {
	if (this->desktopsDirty) {
		this->desktopsDirty = false;
		this->mDesktops->refresh();
	}

	auto candidates = std::move(this->candidates);
	this->candidates.clear();

	for (auto* hwnd: candidates) {
		if (!this->byHwnd.contains(hwnd) && this->isEligible(hwnd)) this->addWindow(hwnd);
	}

	// Handlers may dirty windows again (e.g. a desktop list change); those wait for the next flush.
	auto dirty = std::move(this->dirty);
	this->dirty.clear();

	for (auto iter = dirty.constBegin(); iter != dirty.constEnd(); ++iter) {
		auto* window = this->byHwnd.value(iter.key());
		if (window == nullptr) continue;

		const auto& d = iter.value();

		if (d.eligibility && !this->isEligible(iter.key())) {
			this->removeWindow(window);
			continue;
		}

		Qt::beginPropertyUpdateGroup();
		if (d.title) window->refreshTitle();
		if (d.state) window->refreshState();
		if (d.desktop) window->refreshDesktop();
		// The hosted app's core window only exists once a UWP frame is uncloaked.
		if (d.desktop && window->uwpFrame) window->refreshIdentity();
		Qt::endPropertyUpdateGroup();
	}

	if (this->foregroundDirty) {
		this->foregroundDirty = false;
		this->updateActive();
	}

	emit this->flushed();
}

bool WindowTracker::isEligible(HWND hwnd) const {
	if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
	if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;

	auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	if ((style & WS_CHILD) != 0) return false;

	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	auto appWindow = (exStyle & WS_EX_APPWINDOW) != 0;
	if ((exStyle & WS_EX_TOOLWINDOW) != 0 && !appWindow) return false;

	if (isOwnProcessWindow(hwnd)) return false;

	if (!appWindow) {
		// Alt+Tab lists one window per owner chain: the root owner's last active popup.
		HWND walk = nullptr;
		auto* next = GetAncestor(hwnd, GA_ROOTOWNER);

		while (next != walk) {
			walk = next;
			next = GetLastActivePopup(walk);
			if (IsWindowVisible(next)) break;
		}

		if (walk != hwnd) return false;
	}

	if (isShellClass(windowClass(hwnd))) return false;
	if (GetWindowTextLengthW(hwnd) == 0) return false;

	DWORD cloaked = 0;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

	// Cloaked on the current desktop means hidden (a suspended UWP app for instance); cloaked
	// elsewhere means it lives on another desktop and still counts.
	if (cloaked != 0 && this->mDesktops->isWindowOnCurrent(hwnd)) return false;

	return true;
}

void WindowTracker::addWindow(HWND hwnd) {
	auto* window = new TrackedWindow(this, hwnd);

	Qt::beginPropertyUpdateGroup();
	window->refreshIdentity();
	window->refreshTitle();
	window->refreshState();
	window->refreshDesktop();
	Qt::endPropertyUpdateGroup();

	this->mWindows.append(window);
	this->byHwnd.insert(hwnd, window);

	qCDebug(logTracker) << "Tracking" << window->addressHex() << window->appId() << window->title();
	emit this->windowAdded(window);
}

void WindowTracker::removeWindow(TrackedWindow* window) {
	qCDebug(logTracker) << "Dropping" << window->addressHex() << window->title();

	this->mWindows.removeOne(window);
	this->byHwnd.remove(window->hwnd());
	if (this->mActive == window) this->setActive(nullptr);

	emit this->windowRemoved(window);
	emit window->closed();
	window->deleteLater();
}

void WindowTracker::updateActive() {
	auto* foreground = GetForegroundWindow();

	if (foreground == nullptr) {
		this->setActive(nullptr);
		return;
	}

	// Shell surfaces don't take application focus on compositors; keep the last active window
	// while one of ours is in the foreground.
	if (isOwnProcessWindow(foreground)) return;

	auto* window = this->byHwnd.value(foreground);

	if (window == nullptr && this->isEligible(foreground)) {
		this->addWindow(foreground);
		window = this->byHwnd.value(foreground);
	}

	this->setActive(window);
}

void WindowTracker::setActive(TrackedWindow* window) {
	if (window == this->mActive) return;

	Qt::beginPropertyUpdateGroup();
	if (this->mActive != nullptr) this->mActive->bActivated = false;
	this->mActive = window;
	if (window != nullptr) window->bActivated = true;
	Qt::endPropertyUpdateGroup();

	emit this->activeWindowChanged();
}

void WindowTracker::rescan() {
	QSet<HWND> seen;

	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    auto* seen = reinterpret_cast<QSet<HWND>*>(param); // NOLINT(performance-no-int-to-ptr)
		    seen->insert(hwnd);
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&seen)
	);

	for (auto* window: QList(this->mWindows)) {
		if (!seen.contains(window->hwnd()) || !this->isEligible(window->hwnd())) {
			this->removeWindow(window);
		}
	}

	for (auto* hwnd: seen) {
		if (!this->byHwnd.contains(hwnd) && this->isEligible(hwnd)) this->addWindow(hwnd);
	}

	this->updateActive();
	emit this->flushed();
}

QScreen* WindowTracker::screenFor(HMONITOR monitor) const {
	return this->screensByMonitor.value(monitor);
}

qsizetype WindowTracker::screenIndex(QScreen* screen) {
	return QGuiApplication::screens().indexOf(screen);
}

void WindowTracker::updateScreens() {
	this->screensByMonitor.clear();

	for (auto* screen: QGuiApplication::screens()) {
		auto* monitor = monitorForScreen(screen);
		if (monitor != nullptr) this->screensByMonitor.insert(monitor, screen);
	}
}

void WindowTracker::onScreensChanged() {
	this->updateScreens();
	for (auto* window: this->mWindows) this->dirty[window->hwnd()].state = true;
	this->schedule();
}

void WindowTracker::onDesktopsChanged() {
	// Desktop indices shift when one in the middle is removed.
	for (auto* window: this->mWindows) this->dirty[window->hwnd()].desktop = true;
	this->schedule();
}

} // namespace qs::windows
