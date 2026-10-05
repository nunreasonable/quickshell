#include "input_mask.hpp"
#include <atomic>
#include <cmath>
#include <thread>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpoint.h>
#include <qregion.h>
#include <qtimer.h>
#include <qwindow.h>
#include <windowsx.h>

#include "util.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logInputMask, "quickshell.windows.inputmask", QtWarningMsg);

constexpr UINT WM_QS_CURSOR_MOVED = WM_APP + 1;
constexpr UINT WM_QS_BUTTON_PRESSED = WM_APP + 2;
constexpr auto MESSAGE_WINDOW_CLASS = L"QuickshellInputMaskTracker";

std::atomic<LONG> cursorX = 0;       // NOLINT
std::atomic<LONG> cursorY = 0;       // NOLINT
std::atomic<bool> wakePending = false; // NOLINT
std::atomic<HWND> hookTarget = nullptr; // NOLINT
std::atomic<DWORD> hookThreadId = 0;    // NOLINT
std::atomic<bool> hookInstalled = false; // NOLINT
std::atomic<int> buttonWatchers = 0;     // NOLINT

constexpr DWORD LATE_INPUT_MS = 100;
constexpr int LATE_REPORT_INTERVAL_MS = 10000;
std::atomic<quint32> lateMouseEvents = 0; // NOLINT
std::atomic<quint32> lateKeyEvents = 0;   // NOLINT
std::atomic<DWORD> worstLateMs = 0;       // NOLINT

} // namespace

void noteHookDelay(bool keyboard, DWORD eventTime) {
	auto delay = GetTickCount() - eventTime;
	if (delay < LATE_INPUT_MS || delay > 60000) return;

	(keyboard ? lateKeyEvents : lateMouseEvents).fetch_add(1, std::memory_order_relaxed);

	auto worst = worstLateMs.load(std::memory_order_relaxed);
	while (delay > worst && !worstLateMs.compare_exchange_weak(worst, delay)) {}
}

InputMaskTracker* InputMaskTracker::instance() {
	static QPointer<InputMaskTracker> tracker; // NOLINT

	if (tracker.isNull()) {
		tracker = new InputMaskTracker(QCoreApplication::instance());
	}

	return tracker.data();
}

InputMaskTracker::InputMaskTracker(QObject* parent): QObject(parent) {
	WNDCLASSW wndClass {};
	wndClass.lpfnWndProc = &InputMaskTracker::messageWindowProc;
	wndClass.hInstance = GetModuleHandleW(nullptr);
	wndClass.lpszClassName = MESSAGE_WINDOW_CLASS;
	RegisterClassW(&wndClass);

	this->messageWindow = CreateWindowExW(
	    0,
	    MESSAGE_WINDOW_CLASS,
	    L"",
	    0,
	    0,
	    0,
	    0,
	    0,
	    HWND_MESSAGE,
	    nullptr,
	    wndClass.hInstance,
	    nullptr
	);

	if (this->messageWindow == nullptr) {
		qCWarning(logInputMask) << "Failed to create message window, input masks will be polled.";
	}

	this->pollTimer.setInterval(16);
	QObject::connect(&this->pollTimer, &QTimer::timeout, this, &InputMaskTracker::refresh);

	this->lateReportTimer.setInterval(LATE_REPORT_INTERVAL_MS);
	QObject::connect(&this->lateReportTimer, &QTimer::timeout, this, []() {
		auto mouse = lateMouseEvents.exchange(0);
		auto keys = lateKeyEvents.exchange(0);
		auto worst = worstLateMs.exchange(0);
		if (mouse == 0 && keys == 0) return;

		qCWarning(logInputMask).nospace()
		    << "Input reached the hooks late in the last " << LATE_REPORT_INTERVAL_MS / 1000 << " s: "
		    << mouse << " mouse and " << keys << " keyboard events over " << LATE_INPUT_MS
		    << " ms, the worst " << worst << " ms.";
	});
	this->lateReportTimer.start();
}

InputMaskTracker::~InputMaskTracker() {
	this->stopHook();

	if (this->messageWindow != nullptr) {
		DestroyWindow(this->messageWindow);
		this->messageWindow = nullptr;
	}
}

void InputMaskTracker::setMask(QWindow* window, const QRegion& region) {
	if (window == nullptr) return;

	auto found = false;
	for (auto& entry: this->entries) {
		if (entry.window == window) {
			entry.region = region;
			found = true;
			break;
		}
	}

	if (!found) this->entries.push_back(Entry {.window = window, .region = region});
	qCDebug(logInputMask) << "Mask for" << window << "set to" << region;

	this->updateHookState();
	this->refresh();
}

void InputMaskTracker::remove(QWindow* window) {
	auto removed = this->entries.removeIf([window](const Entry& entry) {
		return entry.window == window || entry.window.isNull();
	});

	if (removed != 0 && window != nullptr) {
		setExStyleBits(hwndOf(window), WS_EX_TRANSPARENT, false);
	}

	this->updateHookState();
}

void InputMaskTracker::refresh() {
	POINT cursor {};
	if (!GetCursorPos(&cursor)) return;
	this->evaluate(cursor);
}

void InputMaskTracker::acquireButtonEvents() {
	buttonWatchers.fetch_add(1);
	this->updateHookState();

	if (!this->hookRunning) {
		qCWarning(logInputMask) << "Mouse hook unavailable, mouse button presses can't be observed.";
	}
}

void InputMaskTracker::releaseButtonEvents() {
	if (buttonWatchers.load() <= 0) return;
	buttonWatchers.fetch_sub(1);
	this->updateHookState();
}

void InputMaskTracker::acquireCursorEvents() {
	this->cursorWatchers++;
	this->updateHookState();
}

void InputMaskTracker::releaseCursorEvents() {
	if (this->cursorWatchers <= 0) return;
	this->cursorWatchers--;
	this->updateHookState();
}

void InputMaskTracker::onButtonPressed(QPoint position, quint32 time) {
	emit this->buttonPressed(position, time);
}

void InputMaskTracker::onCursorMoved() {
	wakePending.store(false);
	POINT cursor {.x = cursorX.load(), .y = cursorY.load()};
	this->evaluate(cursor);
}

void InputMaskTracker::evaluate(POINT cursor) {
	if (this->cursorWatchers > 0) emit this->cursorMoved(QPoint(cursor.x, cursor.y));

	for (auto it = this->entries.begin(); it != this->entries.end();) {
		auto* window = it->window.data();

		if (window == nullptr) {
			it = this->entries.erase(it);
			continue;
		}

		auto* hwnd = hwndOf(window);

		if (hwnd == nullptr || !window->isVisible()) {
			++it;
			continue;
		}

		if (it->region.isEmpty()) {
			setExStyleBits(hwnd, WS_EX_TRANSPARENT, true);
			++it;
			continue;
		}

		RECT rect {};
		if (GetWindowRect(hwnd, &rect) && PtInRect(&rect, cursor)) {
			auto dpr = window->devicePixelRatio();
			auto local = QPoint(
			    static_cast<int>(std::floor((cursor.x - rect.left) / dpr)),
			    static_cast<int>(std::floor((cursor.y - rect.top) / dpr))
			);

			auto through = !it->region.contains(local);
			qCDebug(logInputMask) << "Cursor" << cursor.x << cursor.y << "local" << local << "in" << window
			                      << (through ? "passes through" : "hits the mask");
			setExStyleBits(hwnd, WS_EX_TRANSPARENT, through);
		}

		++it;
	}
}

void InputMaskTracker::updateHookState() {
	auto needed = !this->entries.isEmpty() || buttonWatchers.load() > 0 || this->cursorWatchers > 0;

	if (needed && !this->hookRunning && !this->hookFailed) {
		if (this->messageWindow == nullptr || !this->startHook()) {
			qCWarning(logInputMask) << "Mouse hook unavailable, polling the cursor instead.";
			this->hookFailed = true;
		}
	}

	if (needed && this->hookFailed && (!this->entries.isEmpty() || this->cursorWatchers > 0)
	    && !this->pollTimer.isActive())
	{
		this->pollTimer.start();
	}

	if (!needed) {
		this->stopHook();
		this->pollTimer.stop();
	}
}

bool InputMaskTracker::startHook() {
	if (this->hookRunning) return true;

	auto* ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (ready == nullptr) return false;

	hookTarget.store(this->messageWindow);
	hookInstalled.store(false);
	this->hookThread = std::thread(&InputMaskTracker::hookThreadMain, ready);

	WaitForSingleObject(ready, INFINITE);
	CloseHandle(ready);

	if (!hookInstalled.load()) {
		this->hookThread.join();
		return false;
	}

	this->hookRunning = true;
	qCDebug(logInputMask) << "Mouse hook installed.";
	return true;
}

void InputMaskTracker::stopHook() {
	if (!this->hookRunning) return;

	PostThreadMessageW(hookThreadId.load(), WM_QUIT, 0, 0);
	this->hookThread.join();
	this->hookRunning = false;
	hookTarget.store(nullptr);
}

void InputMaskTracker::hookThreadMain(HANDLE readyEvent) {
	MSG msg {};
	PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	hookThreadId.store(GetCurrentThreadId());

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	auto* hook =
	    SetWindowsHookExW(WH_MOUSE_LL, &InputMaskTracker::mouseHookProc, GetModuleHandleW(nullptr), 0);

	hookInstalled.store(hook != nullptr);
	SetEvent(readyEvent);

	if (hook == nullptr) return;

	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	UnhookWindowsHookEx(hook);
}

LRESULT CALLBACK InputMaskTracker::mouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
	if (code == HC_ACTION) {
		auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam); // NOLINT(performance-no-int-to-ptr)
		noteHookDelay(false, info->time);
		cursorX.store(info->pt.x);
		cursorY.store(info->pt.y);

		if (!wakePending.exchange(true)) {
			auto* target = hookTarget.load();
			if (target != nullptr) PostMessageW(target, WM_QS_CURSOR_MOVED, 0, 0);
			else wakePending.store(false);
		}

		auto button = wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN
		           || wParam == WM_XBUTTONDOWN;

		if (button && buttonWatchers.load() > 0) {
			if (auto* target = hookTarget.load()) {
				PostMessageW(
				    target,
				    WM_QS_BUTTON_PRESSED,
				    static_cast<WPARAM>(info->time),
				    MAKELPARAM(static_cast<WORD>(info->pt.x), static_cast<WORD>(info->pt.y))
				);
			}
		}
	}

	return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK
InputMaskTracker::messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_QS_CURSOR_MOVED) {
		InputMaskTracker::instance()->onCursorMoved();
		return 0;
	}

	if (msg == WM_QS_BUTTON_PRESSED) {
		auto position = QPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		InputMaskTracker::instance()->onButtonPressed(position, static_cast<quint32>(wParam));
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace qs::windows
