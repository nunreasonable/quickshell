#include "hook.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
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
	explicit HookWindow(TrayIconSink sink): sink(std::move(sink)) {}
	~HookWindow() = default;
	Q_DISABLE_COPY_MOVE(HookWindow);

	bool create();
	void announce();

	HWND hwnd = nullptr;

private:
	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);

	LRESULT onCopyData(WPARAM wParam, LPARAM lParam);
	void onTaskbarCreated();
	std::optional<LRESULT> forward(UINT msg, WPARAM wParam, LPARAM lParam);
	HWND explorerWindow();
	void keepFirst();
	void syncGeometry();

	TrayIconSink sink;
	HWND notifyHwnd = nullptr;
	HWND explorer = nullptr;
	RECT lastRect {};
	RECT lastNotifyRect {};
	UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
	ULONGLONG readdGraceUntil = 0;
};

std::mutex gMutex;                  // NOLINT
std::thread gThread;                // NOLINT
std::atomic<HWND> gHwnd = nullptr;  // NOLINT
std::atomic<bool> gStopping = false; // NOLINT

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

	if (wire.message == NIM_SETVERSION) message.version = nid.uVersion;
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
	this->keepFirst();
	SetTimer(this->hwnd, RAISE_TIMER, RAISE_INTERVAL_MS, nullptr);

	return true;
}

// TaskbarCreated to every top level window but ours, the same set HWND_BROADCAST reaches. Our
// own panels would take it for an explorer restart and register their AppBars again.
void HookWindow::announce() {
	this->readdGraceUntil = GetTickCount64() + READD_GRACE_MS;

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
		RemovePropW(this->hwnd, TRAY_HOOK_PROP);
		PostQuitMessage(0);
		return 0;
	default: break;
	}

	if (msg == this->taskbarCreated) {
		this->onTaskbarCreated();
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

void HookWindow::keepFirst() {
	auto* first = FindWindowW(TRAY_CLASS, nullptr);
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
}

// Apps also use FindWindow(L"Shell_TrayWnd") to find out where the taskbar is.
void HookWindow::syncGeometry() {
	auto* target = this->explorerWindow();
	if (target == nullptr) return;

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

void runHook(TrayIconSink sink) {
	SetThreadDescription(GetCurrentThread(), L"qs tray hook");
	// Explorer's rects are physical pixels; ours must be too.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	HookWindow window(std::move(sink));
	if (!window.create()) return;

	gHwnd.store(window.hwnd);

	// stop() came while the window was being created and had nothing to post to.
	if (gStopping.load()) {
		DestroyWindow(window.hwnd);
		gHwnd.store(nullptr);
		return;
	}

	window.announce();

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	gHwnd.store(nullptr);
}

} // namespace

void TrayHook::start(TrayIconSink sink) {
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
	gThread = std::thread(runHook, std::move(sink));
}

void TrayHook::stop() {
	auto lock = std::lock_guard(gMutex);
	if (!gThread.joinable()) return;

	gStopping.store(true);

	auto* hwnd = gHwnd.load();
	if (hwnd != nullptr) PostMessageW(hwnd, WM_CLOSE, 0, 0);

	// It may be waiting on a hung explorer for a few seconds; the process ends anyway.
	if (WaitForSingleObject(gThread.native_handle(), 1000) == WAIT_OBJECT_0) gThread.join();
	else gThread.detach();
}

} // namespace qs::windows::services::systray
