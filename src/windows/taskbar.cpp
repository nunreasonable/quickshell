#include "taskbar.hpp"
#include <atomic>
#include <cwchar>

#include <qt_windows.h>
#include <shellapi.h>

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qfileinfo.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qstring.h>

#include "../core/logcat.hpp"
#include "input_mask.hpp"

namespace qs::windows {

namespace {

QS_LOGGING_CATEGORY(logTaskbar, "quickshell.windows.taskbar", QtWarningMsg);

// Read by restoreForCrash(), from an exception filter.
std::atomic<bool> gConcealing = false;      // NOLINT
std::atomic<bool> gTurnedOnAutoHide = false; // NOLINT

// Physical pixels at the bottom of a monitor that bring the taskbar up, like auto-hide's own.
constexpr int TRIGGER_PX = 2;
// After the cursor left: long enough for auto-hide to slide the taskbar out first.
constexpr qint64 HIDE_DELAY_MS = 700;
constexpr int CHECK_REVEALED_MS = 250;
// While concealed, only to catch explorer showing its windows again (restart, display change).
constexpr int CHECK_CONCEALED_MS = 2000;

bool isTaskbarWindow(HWND hwnd) {
	wchar_t cls[32] {};
	GetClassNameW(hwnd, cls, 32);
	return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

void showAllTaskbars() {
	EnumWindows(
	    [](HWND hwnd, LPARAM /*param*/) -> BOOL {
		    if (isTaskbarWindow(hwnd)) ShowWindow(hwnd, SW_SHOWNA);
		    return TRUE;
	    },
	    0
	);
}

UINT appBarState() {
	APPBARDATA data {};
	data.cbSize = sizeof(data);
	return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &data));
}

void setAutoHide(bool autoHide) {
	APPBARDATA data {};
	data.cbSize = sizeof(data);
	data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
	data.lParam = (appBarState() & ABS_ALWAYSONTOP) | (autoHide ? ABS_AUTOHIDE : 0);
	SHAppBarMessage(ABM_SETSTATE, &data);
}

QRect physicalRect(const RECT& rect) {
	return {QPoint(rect.left, rect.top), QPoint(rect.right - 1, rect.bottom - 1)};
}

} // namespace

TaskbarManager* TaskbarManager::instance() {
	static QPointer<TaskbarManager> manager; // NOLINT

	if (manager.isNull()) {
		// owned by the application: put back on exit even if no QML object is left
		manager = new TaskbarManager(QCoreApplication::instance());
	}

	return manager.data();
}

TaskbarManager::TaskbarManager(QObject* parent): QObject(parent) {
	QObject::connect(&this->checkTimer, &QTimer::timeout, this, &TaskbarManager::onCheck);

	QObject::connect(
	    QCoreApplication::instance(),
	    &QCoreApplication::aboutToQuit,
	    this,
	    &TaskbarManager::disable
	);
}

TaskbarManager::~TaskbarManager() { this->disable(); }

void TaskbarManager::setHoverOnly(bool hoverOnly) {
	if (hoverOnly == this->mHoverOnly) return;
	this->mHoverOnly = hoverOnly;

	if (hoverOnly) this->enable();
	else this->disable();

	emit this->hoverOnlyChanged();
}

void TaskbarManager::restoreForCrash() {
	if (gConcealing.load()) showAllTaskbars();
	if (gTurnedOnAutoHide.load()) setAutoHide(false);
}

void TaskbarManager::enable() {
	if (this->enabled) return;
	this->enabled = true;

	// Hidden taskbar windows still reserve their screen space unless auto-hide is on.
	if ((appBarState() & ABS_AUTOHIDE) == 0) {
		setAutoHide(true);
		gTurnedOnAutoHide.store(true);
		qCInfo(logTaskbar) << "Turned on taskbar auto-hide for hover only mode (undone on exit).";
	}

	this->findBars();

	auto* tracker = InputMaskTracker::instance();
	QObject::connect(tracker, &InputMaskTracker::cursorMoved, this, &TaskbarManager::onCursorMoved);
	tracker->acquireCursorEvents();

	gConcealing.store(true);

	POINT cursor {};
	GetCursorPos(&cursor);
	auto position = QPoint(cursor.x, cursor.y);

	if (this->atTrigger(position) || this->overBar(position)) this->reveal();
	else this->conceal();
}

void TaskbarManager::disable() {
	if (!this->enabled) return;
	this->enabled = false;
	this->revealed = false;
	this->checkTimer.stop();

	auto* tracker = InputMaskTracker::instance();
	QObject::disconnect(tracker, &InputMaskTracker::cursorMoved, this, nullptr);
	tracker->releaseCursorEvents();

	showAllTaskbars();
	gConcealing.store(false);

	if (gTurnedOnAutoHide.exchange(false)) setAutoHide(false);
}

void TaskbarManager::findBars() {
	this->bars.clear();

	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    if (!isTaskbarWindow(hwnd)) return TRUE;

		    auto* self = reinterpret_cast<TaskbarManager*>(param); // NOLINT(performance-no-int-to-ptr)

		    MONITORINFO info {};
		    info.cbSize = sizeof(info);
		    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return TRUE;

		    RECT rect {};
		    GetWindowRect(hwnd, &rect);

		    self->bars.append({
		        .hwnd = hwnd,
		        .monitor = physicalRect(info.rcMonitor),
		        .height = static_cast<int>(rect.bottom - rect.top),
		    });

		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(this)
	);

	qCDebug(logTaskbar) << "Found" << this->bars.length() << "taskbar windows";
}

// The Windows 11 taskbar always sits at the bottom of its monitor.
bool TaskbarManager::atTrigger(QPoint position) const {
	for (const auto& bar: this->bars) {
		if (bar.monitor.contains(position) && position.y() > bar.monitor.bottom() - TRIGGER_PX) {
			return true;
		}
	}

	return false;
}

bool TaskbarManager::overBar(QPoint position) const {
	for (const auto& bar: this->bars) {
		auto shown = QRect(
		    bar.monitor.left(),
		    bar.monitor.bottom() - bar.height + 1,
		    bar.monitor.width(),
		    bar.height
		);

		if (shown.contains(position)) return true;
	}

	return false;
}

// Start, search, the tray overflow, the calendar and quick settings: while one is open, the
// taskbar they belong to stays.
bool TaskbarManager::taskbarPopupActive() {
	auto* foreground = GetForegroundWindow();
	if (foreground == nullptr) return false;
	if (isTaskbarWindow(foreground)) return true;

	DWORD pid = 0;
	GetWindowThreadProcessId(foreground, &pid);

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process == nullptr) return false;

	wchar_t path[MAX_PATH] {};
	DWORD size = MAX_PATH;
	auto ok = QueryFullProcessImageNameW(process, 0, path, &size);
	CloseHandle(process);
	if (!ok) return false;

	auto name = QFileInfo(QString::fromWCharArray(path, static_cast<qsizetype>(size))).fileName().toLower();

	if (name == "shellexperiencehost.exe" || name == "startmenuexperiencehost.exe"
	    || name == "searchhost.exe" || name == "shellhost.exe")
	{
		return true;
	}

	// explorer.exe also runs File Explorer windows; only its menus and tray popups count.
	if (name == "explorer.exe") {
		wchar_t cls[64] {};
		GetClassNameW(foreground, cls, 64);
		return wcscmp(cls, L"#32768") == 0 || wcscmp(cls, L"NotifyIconOverflowWindow") == 0
		    || wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0
		    || wcscmp(cls, L"Xaml_WindowedPopupClass") == 0;
	}

	return false;
}

void TaskbarManager::reveal() {
	if (this->bars.isEmpty()) this->findBars();

	for (const auto& bar: this->bars) ShowWindow(bar.hwnd, SW_SHOWNA);

	this->revealed = true;
	this->leftAt = 0;
	this->checkTimer.start(CHECK_REVEALED_MS);
}

void TaskbarManager::conceal() {
	for (const auto& bar: this->bars) {
		if (IsWindowVisible(bar.hwnd)) ShowWindow(bar.hwnd, SW_HIDE);
	}

	this->revealed = false;
	this->leftAt = 0;
	this->checkTimer.start(CHECK_CONCEALED_MS);
}

void TaskbarManager::onCursorMoved(QPoint position) {
	if (!this->enabled) return;

	if (!this->revealed) {
		if (this->atTrigger(position)) this->reveal();
		return;
	}

	if (this->overBar(position) || this->atTrigger(position)) this->leftAt = 0;
	else if (this->leftAt == 0) this->leftAt = QDateTime::currentMSecsSinceEpoch();
}

void TaskbarManager::onCheck() {
	if (!this->enabled) return;

	auto stale = this->bars.isEmpty();
	for (const auto& bar: this->bars) stale = stale || !IsWindow(bar.hwnd);
	if (stale) this->findBars(); // explorer restarted

	if (!this->revealed) {
		// Explorer shows its taskbars again on its own now and then.
		for (const auto& bar: this->bars) {
			if (IsWindowVisible(bar.hwnd)) ShowWindow(bar.hwnd, SW_HIDE);
		}

		return;
	}

	POINT cursor {};
	GetCursorPos(&cursor);
	auto position = QPoint(cursor.x, cursor.y);

	if (this->overBar(position) || this->atTrigger(position) || taskbarPopupActive()) {
		this->leftAt = 0;
		return;
	}

	auto now = QDateTime::currentMSecsSinceEpoch();
	if (this->leftAt == 0) this->leftAt = now;
	else if (now - this->leftAt >= HIDE_DELAY_MS) this->conceal();
}

Taskbar::Taskbar(QObject* parent): QObject(parent) {
	QObject::connect(
	    TaskbarManager::instance(),
	    &TaskbarManager::hoverOnlyChanged,
	    this,
	    &Taskbar::hoverOnlyChanged
	);
}

bool Taskbar::hoverOnly() const { return TaskbarManager::instance()->hoverOnly(); }
void Taskbar::setHoverOnly(bool hoverOnly) { TaskbarManager::instance()->setHoverOnly(hoverOnly); }

} // namespace qs::windows
