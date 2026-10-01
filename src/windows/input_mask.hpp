#pragma once

#include <thread>

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qregion.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qwindow.h>

namespace qs::windows {

// Input-only window masks.
//
// Windows has no per-pixel input region for top level windows: SetWindowRgn (QWindow::setMask)
// clips the visuals as well, and WM_NCHITTEST cannot forward input to another process.
// Instead, WS_EX_TRANSPARENT (click-through) is toggled on each masked window depending on
// whether the cursor is inside its mask. The cursor is observed with a WH_MOUSE_LL hook that
// runs on a dedicated thread and only records the position, since a slow hook delays the whole
// system's mouse and Windows removes hooks that stall.
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

	static LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam);
	static void hookThreadMain(HANDLE readyEvent);

	QList<Entry> entries;
	HWND messageWindow = nullptr;
	std::thread hookThread;
	bool hookRunning = false;
	QTimer pollTimer;
};

} // namespace qs::windows
