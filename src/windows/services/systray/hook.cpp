#include "hook.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <qt_windows.h>
#include <shellapi.h>

#include <qimage.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <quuid.h>

#include "../../../core/logcat.hpp"
#include "../../util.hpp"

namespace qs::windows::services::systray {

namespace {

QS_LOGGING_CATEGORY(logTrayHook, "quickshell.windows.systray", QtWarningMsg);

constexpr const wchar_t* TRAY_CLASS = L"Shell_TrayWnd";
// Explorer's tray window has this child and a few apps look it up to find the notification area.
constexpr const wchar_t* NOTIFY_CLASS = L"TrayNotifyWnd";
// shell32 finds the tray by class alone, but tools that look for explorer's taskbar often ask
// for its empty title too: with a title of our own they get explorer's window instead of ours.
// Checked at start (probeTitle) and dropped if Shell_NotifyIcon stops reaching us with it.
constexpr const wchar_t* HOOK_TITLE = L"Quickshell tray hook";
// The icon id of that check, on our own window: no app can have it.
constexpr UINT PROBE_UID = 0x51535459;

// COPYDATASTRUCT::dwData shell32 uses for Shell_NotifyIcon (0 is SHAppBarMessage, 3 is
// Shell_NotifyIconGetRect: both only forwarded).
constexpr ULONG_PTR COPYDATA_NOTIFYICON = 1;
constexpr DWORD NOTIFYICON_SIGNATURE = 0x34753423;

constexpr UINT_PTR RAISE_TIMER = 1;
constexpr UINT_PTR ANNOUNCE_TIMER = 2;
// While explorer's window is above ours (explorer raises it whenever the taskbar is used), icon
// updates go straight to explorer and we miss them, so this is kept short. FindWindow is cheap.
constexpr UINT RAISE_INTERVAL_MS = 100;
// After explorer restarts: its own TaskbarCreated reached apps while its new window was still
// above ours.
constexpr UINT ANNOUNCE_DELAY_MS = 1500;
// shell32 gives up on the tray after a few seconds too.
constexpr UINT FORWARD_TIMEOUT_MS = 4000;
// How long after our TaskbarCreated a NIM_ADD explorer refuses is taken for a re-add.
constexpr ULONGLONG READD_GRACE_MS = 30000;
// Brief mode: how long the window stays in front after asking apps to add their icons again.
// Apps answer TaskbarCreated within a moment; a busy one may take a few seconds.
constexpr ULONGLONG BRIEF_FRONT_MS = 8000;

// NOTIFYICONDATAW as shell32 sends it to the tray: handles cut to 32 bits so 32 and 64 bit
// processes share one format (sign extending them back is how Windows shares handles between
// the two). Only 4 byte members, so the layout is the same in a 64 bit build.
struct NotifyIconWire {
	DWORD cbSize;
	DWORD hWnd;
	UINT uID;
	UINT uFlags;
	UINT uCallbackMessage;
	DWORD hIcon;
	WCHAR szTip[128];
	DWORD dwState;
	DWORD dwStateMask;
	WCHAR szInfo[256];
	UINT uVersion; // union with uTimeout
	WCHAR szInfoTitle[64];
	DWORD dwInfoFlags;
	GUID guidItem;
	DWORD hBalloonIcon;
};

static_assert(sizeof(NotifyIconWire) == 956, "the 32 bit NOTIFYICONDATAW is 956 bytes");

struct TrayDataWire {
	DWORD signature;
	DWORD message;
	NotifyIconWire nid;
};

// Enough to read identity, flags and callback; older apps send shorter structures.
constexpr size_t MIN_TRAY_DATA = offsetof(TrayDataWire, nid) + offsetof(NotifyIconWire, szTip);

template <typename T>
T handleFromWire(DWORD value) {
	// NOLINTNEXTLINE(performance-no-int-to-ptr)
	return reinterpret_cast<T>(static_cast<LONG_PTR>(static_cast<LONG>(value)));
}

bool processIsElevated() {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

	TOKEN_ELEVATION elevation {};
	DWORD size = 0;
	auto ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
	CloseHandle(token);

	return ok && elevation.TokenIsElevated != 0;
}

class HookWindow {
public:
	explicit HookWindow(TrayIconSink sink, std::function<void()> missed)
	    : sink(std::move(sink))
	    , missed(std::move(missed)) {}
	~HookWindow() = default;
	Q_DISABLE_COPY_MOVE(HookWindow);

	bool create();
	void probeTitle();
	void announce();

	HWND hwnd = nullptr;

private:
	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static void CALLBACK onWinEvent(
	    HWINEVENTHOOK hook,
	    DWORD event,
	    HWND hwnd,
	    LONG idObject,
	    LONG idChild,
	    DWORD thread,
	    DWORD time
	);
	LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);

	LRESULT onCopyData(WPARAM wParam, LPARAM lParam);
	void onTaskbarCreated();
	void announceQueued();
	std::optional<LRESULT> forward(UINT msg, WPARAM wParam, LPARAM lParam);
	HWND explorerWindow();
	void keepFirst(bool report = true);
	void stepBack(HWND first);
	void syncGeometry();
	void mirrorProps();
	void watchExplorer(HWND target);
	void unwatchExplorer();

	TrayIconSink sink;
	std::function<void()> missed;
	HWND notifyHwnd = nullptr;
	HWND explorer = nullptr;
	RECT lastRect {};
	RECT lastNotifyRect {};
	UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
	ULONGLONG readdGraceUntil = 0;
	ULONGLONG propsSyncedAt = 0;
	// Names (or "#<atom>") of the explorer properties set on this window.
	std::vector<std::wstring> mirrored;
	bool probing = false;
	bool probeSeen = false;
	DWORD watchedPid = 0;
	HWINEVENTHOOK foregroundHook = nullptr;
	HWINEVENTHOOK showHook = nullptr;
};

std::mutex gMutex;                  // NOLINT
std::thread gThread;                // NOLINT
std::atomic<HWND> gHwnd = nullptr;  // NOLINT
std::atomic<bool> gStopping = false; // NOLINT
std::atomic<bool> gStarted = false;  // NOLINT
// Brief mode (see TrayHook::start): only in front of explorer's window until this time.
std::atomic<bool> gBrief = false;         // NOLINT
std::atomic<ULONGLONG> gFrontUntil = 0;   // NOLINT

void stayInFront(ULONGLONG ms) {
	auto until = GetTickCount64() + ms;
	auto current = gFrontUntil.load();
	while (current < until && !gFrontUntil.compare_exchange_weak(current, until)) {}
}

bool wantFront() { return !gBrief.load() || GetTickCount64() < gFrontUntil.load(); }
// Only touched on the hook thread (WinEvent callbacks have no user data).
HookWindow* gWindow = nullptr; // NOLINT

std::mutex gAnnounceMutex;          // NOLINT
std::vector<HWND> gAnnounceQueue;   // NOLINT

// Posted to the hook window by announceTo(). Registered, so it can't be taken for one of
// explorer's private messages (those are forwarded).
UINT announceMessage() {
	static const UINT message = RegisterWindowMessageW(L"QuickshellTrayHookAnnounce"); // NOLINT
	return message;
}

TrayIconMessage decode(const TrayDataWire& wire) {
	const auto& nid = wire.nid;

	TrayIconMessage message;
	message.message = wire.message;
	message.hwnd = handleFromWire<HWND>(nid.hWnd);
	message.uid = nid.uID;
	message.flags = nid.uFlags;
	message.callbackMessage = nid.uCallbackMessage;
	message.state = nid.dwState;
	message.stateMask = nid.dwStateMask;

	if ((nid.uFlags & NIF_ICON) != 0) {
		auto* icon = handleFromWire<HICON>(nid.hIcon);
		if (icon != nullptr) message.icon = QImage::fromHICON(icon);
	}

	if ((nid.uFlags & NIF_TIP) != 0) {
		auto length = std::find(std::begin(nid.szTip), std::end(nid.szTip), L'\0') - std::begin(nid.szTip);
		message.tip = QString::fromWCharArray(nid.szTip, length);
	}

	// Outside NIM_SETVERSION it's whatever the app's structure holds (see SystemTrayItem::update).
	message.version = nid.uVersion;
	if ((nid.uFlags & NIF_GUID) != 0) message.guid = QUuid(nid.guidItem);

	return message;
}

bool HookWindow::create() {
	auto* instance = GetModuleHandleW(nullptr);

	WNDCLASSEXW trayClass {};
	trayClass.cbSize = sizeof(trayClass);
	trayClass.lpfnWndProc = &HookWindow::wndProc;
	trayClass.hInstance = instance;
	trayClass.lpszClassName = TRAY_CLASS;

	WNDCLASSEXW notifyClass {};
	notifyClass.cbSize = sizeof(notifyClass);
	notifyClass.lpfnWndProc = &DefWindowProcW;
	notifyClass.hInstance = instance;
	notifyClass.lpszClassName = NOTIFY_CLASS;

	if (RegisterClassExW(&trayClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		qCWarning(logTrayHook) << "Could not register the tray window class:" << GetLastError();
		return false;
	}

	RegisterClassExW(&notifyClass);

	// Never visible: layered with zero alpha and click-through in case something shows it
	// anyway (apps call ShowWindow on whatever FindWindow gave them to show the taskbar).
	this->hwnd = CreateWindowExW(
	    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
	    TRAY_CLASS,
	    L"",
	    WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
	    0,
	    0,
	    0,
	    0,
	    nullptr,
	    nullptr,
	    instance,
	    this
	);

	if (this->hwnd == nullptr) {
		qCWarning(logTrayHook) << "Could not create the tray window:" << GetLastError();
		return false;
	}

	SetLayeredWindowAttributes(this->hwnd, 0, 0, LWA_ALPHA);
	SetPropW(this->hwnd, TRAY_HOOK_PROP, reinterpret_cast<HANDLE>(1)); // NOLINT

	this->notifyHwnd = CreateWindowExW(
	    0,
	    NOTIFY_CLASS,
	    L"",
	    WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
	    0,
	    0,
	    0,
	    0,
	    this->hwnd,
	    nullptr,
	    instance,
	    nullptr
	);

	this->syncGeometry();
	// Nothing to have missed yet: the first seed from explorer is on its way.
	this->keepFirst(false);
	SetTimer(this->hwnd, RAISE_TIMER, RAISE_INTERVAL_MS, nullptr);

	return true;
}

// Checks that shell32 still finds this window with HOOK_TITLE set, with a Shell_NotifyIcon call
// of our own for an icon nobody has (answered here, never passed on). Called from the hook
// thread itself, so shell32's SendMessage reaches handle() directly.
void HookWindow::probeTitle() {
	SetWindowTextW(this->hwnd, HOOK_TITLE);

	NOTIFYICONDATAW nid {};
	nid.cbSize = sizeof(nid);
	nid.hWnd = this->hwnd;
	nid.uID = PROBE_UID;
	nid.uFlags = NIF_STATE;

	this->probing = true;
	this->probeSeen = false;
	Shell_NotifyIconW(NIM_MODIFY, &nid);
	this->probing = false;

	if (this->probeSeen) return;

	qCInfo(logTrayHook) << "Shell_NotifyIcon didn't reach the tray hook with a window title of its"
	                    << "own, using explorer's empty one";
	SetWindowTextW(this->hwnd, L"");
}

// TaskbarCreated to every top level window but ours, the same set HWND_BROADCAST reaches. Our
// own panels would take it for an explorer restart and register their AppBars again.
void HookWindow::announce() {
	this->readdGraceUntil = GetTickCount64() + READD_GRACE_MS;
	stayInFront(BRIEF_FRONT_MS);
	this->keepFirst(false);

	EnumWindows(
	    [](HWND window, LPARAM message) -> BOOL {
		    if (!isOwnProcessWindow(window)) {
			    SendNotifyMessageW(window, static_cast<UINT>(message), 0, 0);
		    }
		    return TRUE;
	    },
	    static_cast<LPARAM>(this->taskbarCreated)
	);

	qCDebug(logTrayHook) << "Sent TaskbarCreated";
}

// Targeted TaskbarCreated for icon owners whose callback message is still unknown: the owners
// of icons explorer had before us, which the broadcast misses when they are message-only
// windows, and icons added while explorer's window was in front of ours.
void HookWindow::announceQueued() {
	std::vector<HWND> owners;
	{
		auto lock = std::lock_guard(gAnnounceMutex);
		owners.swap(gAnnounceQueue);
	}

	std::ranges::sort(owners);
	auto [first, last] = std::ranges::unique(owners);
	owners.erase(first, last);
	if (owners.empty()) return;

	// Their answer has to come through here.
	stayInFront(BRIEF_FRONT_MS);
	this->keepFirst();
	this->readdGraceUntil = GetTickCount64() + READD_GRACE_MS;

	auto sent = 0;
	for (auto* owner: owners) {
		if (IsWindow(owner) == 0 || isOwnProcessWindow(owner)) continue;
		SendNotifyMessageW(owner, this->taskbarCreated, 0, 0);
		sent++;
	}

	qCDebug(logTrayHook) << "Sent TaskbarCreated to" << sent << "icon owners";
}

LRESULT CALLBACK HookWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_NCCREATE) {
		auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam); // NOLINT
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	auto* self = reinterpret_cast<HookWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); // NOLINT
	if (self == nullptr || self->hwnd != hwnd) return DefWindowProcW(hwnd, msg, wParam, lParam);

	return self->handle(msg, wParam, lParam);
}

void CALLBACK HookWindow::onWinEvent(
    HWINEVENTHOOK /*hook*/,
    DWORD /*event*/,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr) return;

	auto* self = gWindow;
	if (self == nullptr || hwnd != self->explorer) return;

	self->keepFirst();
}

LRESULT HookWindow::handle(UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_COPYDATA: return this->onCopyData(wParam, lParam);
	case WM_TIMER:
		if (wParam == RAISE_TIMER) {
			this->keepFirst();
			this->syncGeometry();
		} else if (wParam == ANNOUNCE_TIMER) {
			KillTimer(this->hwnd, ANNOUNCE_TIMER);
			this->announce();
		}
		return 0;
	case WM_WINDOWPOSCHANGING: {
		auto* pos = reinterpret_cast<WINDOWPOS*>(lParam); // NOLINT
		pos->flags &= ~SWP_SHOWWINDOW;
		break;
	}
	case WM_WINDOWPOSCHANGED:
		if (IsWindowVisible(this->hwnd)) {
			SetWindowLongPtrW(
			    this->hwnd,
			    GWL_STYLE,
			    GetWindowLongPtrW(this->hwnd, GWL_STYLE) & ~static_cast<LONG_PTR>(WS_VISIBLE)
			);
		}
		break;
	case WM_CLOSE:
		if (gStopping.load()) {
			DestroyWindow(this->hwnd);
			return 0;
		}

		// Explorer's answer to WM_CLOSE is the shut down dialog; whoever sent it meant that one.
		return this->forward(msg, wParam, lParam).value_or(0);
	case WM_DESTROY:
		KillTimer(this->hwnd, RAISE_TIMER);
		KillTimer(this->hwnd, ANNOUNCE_TIMER);
		this->unwatchExplorer();
		RemovePropW(this->hwnd, TRAY_HOOK_PROP);
		PostQuitMessage(0);
		return 0;
	default: break;
	}

	if (msg == this->taskbarCreated) {
		this->onTaskbarCreated();
		return 0;
	}

	if (msg == announceMessage()) {
		this->announceQueued();
		return 0;
	}

	// The taskbar's own commands and private messages (shell32 posts some, apps send others).
	if (msg == WM_COMMAND || msg >= WM_USER) {
		return this->forward(msg, wParam, lParam).value_or(0);
	}

	return DefWindowProcW(this->hwnd, msg, wParam, lParam);
}

LRESULT HookWindow::onCopyData(WPARAM wParam, LPARAM lParam) {
	auto* data = reinterpret_cast<COPYDATASTRUCT*>(lParam); // NOLINT

	if (data == nullptr || data->dwData != COPYDATA_NOTIFYICON || data->lpData == nullptr
	    || data->cbData < MIN_TRAY_DATA)
	{
		return this->forward(WM_COPYDATA, wParam, lParam).value_or(0);
	}

	TrayDataWire wire {};
	std::memcpy(&wire, data->lpData, std::min<size_t>(data->cbData, sizeof(wire)));

	if (wire.signature != NOTIFYICON_SIGNATURE) {
		return this->forward(WM_COPYDATA, wParam, lParam).value_or(0);
	}

	// Decoded before forwarding: the app may destroy its HICON as soon as the call returns.
	auto message = decode(wire);

	// Our probe, or another Quickshell process's that reached this window first: not an icon.
	if (message.uid == PROBE_UID && isTrayHookWindow(message.hwnd)) {
		if (this->probing && message.hwnd == this->hwnd) this->probeSeen = true;
		return FALSE;
	}

	LRESULT answer = FALSE;

	if (this->explorerWindow() == nullptr) {
		// No explorer tray (it crashed, or another shell): we are the only tray, so accept.
		answer = wire.message != NIM_SETVERSION || wire.nid.uVersion <= NOTIFYICON_VERSION_4;
	} else {
		answer = this->forward(WM_COPYDATA, wParam, lParam).value_or(FALSE);

		// Apps answer our TaskbarCreated with NIM_ADD for icons explorer still has, which it
		// refuses. To the app that is a failed re-add after a "restart", so make it the update
		// it meant.
		if (wire.message == NIM_ADD && answer == FALSE && GetTickCount64() < this->readdGraceUntil) {
			std::vector<char> copy(data->cbData);
			std::memcpy(copy.data(), data->lpData, data->cbData);

			DWORD modify = NIM_MODIFY;
			std::memcpy(copy.data() + offsetof(TrayDataWire, message), &modify, sizeof(modify));

			auto retry = *data;
			retry.lpData = copy.data();

			answer = this->forward(WM_COPYDATA, wParam, reinterpret_cast<LPARAM>(&retry)).value_or(FALSE);
		}
	}

	this->sink(std::move(message));
	return answer;
}

void HookWindow::onTaskbarCreated() {
	// Our own announcement doesn't come here (announce() skips this process), but another
	// Quickshell process's does: only a new explorer window means explorer restarted.
	auto* previous = this->explorer;
	this->explorer = explorerTaskbarWindow();
	if (this->explorer == previous || this->explorer == nullptr) return;

	qCInfo(logTrayHook) << "Explorer restarted, asking apps for their tray icons again";
	// Its own TaskbarCreated is already out: the apps' answers should come here too.
	stayInFront(ANNOUNCE_DELAY_MS + BRIEF_FRONT_MS);
	this->propsSyncedAt = 0;
	this->lastRect = {};
	this->lastNotifyRect = {};
	this->keepFirst();
	SetTimer(this->hwnd, ANNOUNCE_TIMER, ANNOUNCE_DELAY_MS, nullptr);
}

std::optional<LRESULT> HookWindow::forward(UINT msg, WPARAM wParam, LPARAM lParam) {
	auto* target = this->explorerWindow();
	if (target == nullptr) return std::nullopt;

	auto kind = InSendMessageEx(nullptr) & (ISMEX_SEND | ISMEX_NOTIFY | ISMEX_CALLBACK);

	// Posted to us: post it on. Its result is never read.
	if (kind == ISMEX_NOSEND) {
		PostMessageW(target, msg, wParam, lParam);
		return 0;
	}

	if ((kind & ISMEX_NOTIFY) != 0) {
		SendNotifyMessageW(target, msg, wParam, lParam);
		return 0;
	}

	DWORD_PTR result = 0;
	if (SendMessageTimeoutW(
	        target,
	        msg,
	        wParam,
	        lParam,
	        SMTO_NORMAL | SMTO_ABORTIFHUNG,
	        FORWARD_TIMEOUT_MS,
	        &result
	    )
	    == 0)
	{
		qCDebug(logTrayHook) << "Explorer didn't answer message" << msg << GetLastError();
		return std::nullopt;
	}

	return static_cast<LRESULT>(result);
}

HWND HookWindow::explorerWindow() {
	if (this->explorer == nullptr || !IsWindow(this->explorer)) {
		this->explorer = explorerTaskbarWindow();
	}

	return this->explorer;
}

void HookWindow::keepFirst(bool report) {
	auto* first = FindWindowW(TRAY_CLASS, nullptr);

	if (!wantFront()) {
		this->stepBack(first);
		return;
	}

	if (first == this->hwnd || first == nullptr) return;

	// Another Quickshell process got there first: one hook is enough, don't fight over it.
	if (isTrayHookWindow(first)) return;

	qCDebug(logTrayHook) << "Explorer's tray window went above ours, raising";

	SetWindowPos(
	    this->hwnd,
	    HWND_TOPMOST,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
	);

	// Icon changes apps sent meanwhile reached explorer only; the host reads explorer's list
	// again to catch up.
	if (report && this->missed) this->missed();
}

// Brief mode outside of its moments in front: right behind explorer's window, so FindWindow
// finds explorer's and only the host's reads of explorer's list see icon changes.
void HookWindow::stepBack(HWND first) {
	if (first != this->hwnd) return;

	// No explorer tray: we are the only one, and stay.
	auto* target = this->explorerWindow();
	if (target == nullptr) return;

	SetWindowPos(
	    this->hwnd,
	    target,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
	);
}

// Explorer raises its taskbar when it's activated or shown (the taskbar was clicked, auto-hide
// or ii brought it up). Hearing about it right away keeps the time apps' updates go past us
// short; the raise timer stays as the safety net. Only explorer's own events are asked for.
void HookWindow::watchExplorer(HWND target) {
	DWORD pid = 0;
	if (target != nullptr) GetWindowThreadProcessId(target, &pid);
	if (pid == this->watchedPid) return;

	this->unwatchExplorer();
	this->watchedPid = pid;
	if (pid == 0) return;

	this->foregroundHook = SetWinEventHook(
	    EVENT_SYSTEM_FOREGROUND,
	    EVENT_SYSTEM_FOREGROUND,
	    nullptr,
	    &HookWindow::onWinEvent,
	    pid,
	    0,
	    WINEVENT_OUTOFCONTEXT
	);

	this->showHook = SetWinEventHook(
	    EVENT_OBJECT_SHOW,
	    EVENT_OBJECT_SHOW,
	    nullptr,
	    &HookWindow::onWinEvent,
	    pid,
	    0,
	    WINEVENT_OUTOFCONTEXT
	);
}

void HookWindow::unwatchExplorer() {
	if (this->foregroundHook != nullptr) UnhookWinEvent(this->foregroundHook);
	if (this->showHook != nullptr) UnhookWinEvent(this->showHook);
	this->foregroundHook = nullptr;
	this->showHook = nullptr;
	this->watchedPid = 0;
}

// Windows' own code also finds the taskbar with FindWindow(L"Shell_TrayWnd") and then reads
// the window properties explorer puts on it: ITaskbarList (taskbar progress and overlays, which
// WPF apps use) follows "TaskbandHWND" to the taskbar buttons and failed with E_NOTIMPL on ours,
// and OLE drag and drop and the DPI of the taskbar are found the same way. So this window carries
// copies of all of them, kept up to date.
void HookWindow::mirrorProps() {
	auto* target = this->explorerWindow();
	if (target == nullptr) return;

	struct Found {
		std::vector<std::pair<std::wstring, HANDLE>> props;
	} found;

	EnumPropsExW(
	    target,
	    [](HWND /*hwnd*/, LPWSTR name, HANDLE value, ULONG_PTR param) -> BOOL {
		    auto* found = reinterpret_cast<Found*>(param); // NOLINT(performance-no-int-to-ptr)
		    auto key = IS_INTRESOURCE(name)
		                 ? L"#" + std::to_wstring(reinterpret_cast<ULONG_PTR>(name)) // NOLINT
		                 : std::wstring(name);
		    found->props.emplace_back(std::move(key), value);
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&found)
	);

	auto nameOf = [](const std::wstring& key) {
		return key.starts_with(L'#') ? MAKEINTATOM(std::stoul(key.substr(1))) : key.c_str();
	};

	std::vector<std::wstring> current;
	for (const auto& [key, value]: found.props) {
		if (key == TRAY_HOOK_PROP) continue;
		SetPropW(this->hwnd, nameOf(key), value);
		current.push_back(key);
	}

	for (const auto& key: this->mirrored) {
		if (std::ranges::find(current, key) == current.end()) RemovePropW(this->hwnd, nameOf(key));
	}

	this->mirrored = std::move(current);
	this->propsSyncedAt = GetTickCount64();
}

// Apps also use FindWindow(L"Shell_TrayWnd") to find out where the taskbar is.
void HookWindow::syncGeometry() {
	auto* target = this->explorerWindow();
	this->watchExplorer(target);
	if (target == nullptr) return;

	if (GetTickCount64() - this->propsSyncedAt >= 1000) this->mirrorProps();

	RECT rect {};
	if (!GetWindowRect(target, &rect)) return;

	if (!EqualRect(&rect, &this->lastRect)) {
		this->lastRect = rect;
		SetWindowPos(
		    this->hwnd,
		    nullptr,
		    rect.left,
		    rect.top,
		    rect.right - rect.left,
		    rect.bottom - rect.top,
		    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER
		);
	}

	auto* notify = FindWindowExW(target, nullptr, NOTIFY_CLASS, nullptr);
	RECT notifyRect {};
	if (this->notifyHwnd == nullptr || notify == nullptr || !GetWindowRect(notify, &notifyRect)) {
		return;
	}

	OffsetRect(&notifyRect, -rect.left, -rect.top);

	if (!EqualRect(&notifyRect, &this->lastNotifyRect)) {
		this->lastNotifyRect = notifyRect;
		SetWindowPos(
		    this->notifyHwnd,
		    nullptr,
		    notifyRect.left,
		    notifyRect.top,
		    notifyRect.right - notifyRect.left,
		    notifyRect.bottom - notifyRect.top,
		    SWP_NOZORDER | SWP_NOACTIVATE
		);
	}
}

void runHook(TrayIconSink sink, std::function<void()> missed) {
	SetThreadDescription(GetCurrentThread(), L"qs tray hook");
	// Explorer's rects are physical pixels; ours must be too.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// In front for the start in brief mode too: the title probe and the apps' answers to the
	// first TaskbarCreated need it.
	stayInFront(BRIEF_FRONT_MS);

	HookWindow window(std::move(sink), std::move(missed));
	if (!window.create()) return;

	gWindow = &window;
	gHwnd.store(window.hwnd);

	// stop() came while the window was being created and had nothing to post to.
	if (gStopping.load()) {
		DestroyWindow(window.hwnd);
		gHwnd.store(nullptr);
		gWindow = nullptr;
		return;
	}

	window.probeTitle();
	window.announce();

	// announceTo() calls that came before the window existed.
	PostMessageW(window.hwnd, announceMessage(), 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	gHwnd.store(nullptr);
	gWindow = nullptr;
}

} // namespace

void TrayHook::start(TrayIconSink sink, std::function<void()> missedTraffic, bool brief) {
	auto lock = std::lock_guard(gMutex);
	if (gThread.joinable()) return;

	// Medium integrity apps couldn't reach an elevated window (UIPI drops their WM_COPYDATA),
	// and it would be the window they find: their icons would vanish from explorer too.
	if (processIsElevated()) {
		qCWarning(logTrayHook) << "Running elevated: not hooking the system tray, so apps keep"
		                       << "reaching explorer's. Tray icons can't be clicked from the shell.";
		return;
	}

	gStopping.store(false);
	gStarted.store(true);
	gBrief.store(brief);
	gThread = std::thread(runHook, std::move(sink), std::move(missedTraffic));
}

void TrayHook::stop() {
	auto lock = std::lock_guard(gMutex);
	if (!gThread.joinable()) return;

	gStopping.store(true);
	gStarted.store(false);

	auto* hwnd = gHwnd.load();
	if (hwnd != nullptr) PostMessageW(hwnd, WM_CLOSE, 0, 0);

	// It may be waiting on a hung explorer for a few seconds; the process ends anyway.
	if (WaitForSingleObject(gThread.native_handle(), 1000) == WAIT_OBJECT_0) gThread.join();
	else gThread.detach();
}

void TrayHook::announceTo(std::vector<HWND> owners) {
	// Without the hook their answer would only go to explorer again.
	if (owners.empty() || !gStarted.load()) return;

	{
		auto lock = std::lock_guard(gAnnounceMutex);
		// Bounded in case the window never came up to take them.
		if (gAnnounceQueue.size() > 1024) gAnnounceQueue.clear();
		gAnnounceQueue.insert(gAnnounceQueue.end(), owners.begin(), owners.end());
	}

	// Not running yet: runHook() looks at the queue once the window exists.
	auto* hwnd = gHwnd.load();
	if (hwnd != nullptr) PostMessageW(hwnd, announceMessage(), 0, 0);
}

} // namespace qs::windows::services::systray
