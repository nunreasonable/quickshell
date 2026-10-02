#pragma once

#include <thread>

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qpoint.h>
#include <qpointer.h>
#include <qregion.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qwindow.h>

namespace qs::windows {

// From the low level hook threads: notes how long after it was generated an input event reached
// the hook. InputMaskTracker logs a warning when events keep arriving late, so a laggy system
// shows up in `qs log` together with whether the delay happens before ii's hooks.
void noteHookDelay(bool keyboard, DWORD eventTime);

// Input-only window masks.
//
// Windows has no per-pixel input region for top level windows: SetWindowRgn (QWindow::setMask)
// clips the visuals as well, and WM_NCHITTEST cannot forward input to another process.
// Instead, WS_EX_TRANSPARENT (click-through) is toggled on each masked window depending on
// whether the cursor is inside its mask. The cursor is observed with a WH_MOUSE_LL hook that
// runs on a dedicated thread and only records the position, since a slow hook delays the whole
// system's mouse and Windows removes hooks that stall.
//
// The same hook reports mouse button presses (for focus grabs) while someone asks for them,
// so the process never installs a second mouse hook.
class InputMaskTracker: public QObject {
	Q_OBJECT;

public:
	~InputMaskTracker() override;
	Q_DISABLE_COPY_MOVE(InputMaskTracker);

	static InputMaskTracker* instance();

	// Only `region` (window coordinates) accepts input. An empty region makes the window
	// fully click-through.
	void setMask(QWindow* window, const QRegion& region);
	void remove(QWindow* window);

	// Re-applies styles for the current cursor position, e.g. after a window moved or Qt
	// rewrote its window styles.
	void refresh();

	// Reference counted: buttonPressed is emitted while at least one user acquired it.
	void acquireButtonEvents();
	void releaseButtonEvents();

	// Reference counted, like button events: cursorMoved is emitted while at least one user
	// acquired it (the mouse hook runs for them even without any input mask).
	void acquireCursorEvents();
	void releaseCursorEvents();

signals:
	// A mouse button went down anywhere, in physical screen coordinates. `time` is the
	// event's GetTickCount time. Emitted after the input masks saw the matching cursor move.
	void buttonPressed(QPoint position, quint32 time);
	// The cursor moved, in physical screen coordinates. Coalesced: at most once per event loop
	// turn however fast the mouse goes.
	void cursorMoved(QPoint position);

private:
	explicit InputMaskTracker(QObject* parent);

	struct Entry {
		QPointer<QWindow> window;
		QRegion region;
	};

	void evaluate(POINT cursor);
	void updateHookState();
	bool startHook();
	void stopHook();
	void onCursorMoved();
	void onButtonPressed(QPoint position, quint32 time);

	static LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam);
	static void hookThreadMain(HANDLE readyEvent);

	QList<Entry> entries;
	HWND messageWindow = nullptr;
	std::thread hookThread;
	bool hookRunning = false;
	bool hookFailed = false;
	int cursorWatchers = 0;
	QTimer pollTimer;
	QTimer lateReportTimer;
};

} // namespace qs::windows
