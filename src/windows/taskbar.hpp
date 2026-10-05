#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qpoint.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

// Which edge of its monitor a taskbar is docked to. Windows 11 always uses Bottom; Windows 10
// (and any version, via drag-and-drop) allows any of the four.
enum class TaskbarEdge : quint8 { Left, Top, Right, Bottom };

// Process wide owner of the taskbar's visibility, so a config reload doesn't flash it.
//
// Windows' own auto-hide also brings the taskbar up when it becomes the foreground window (the
// last app closed and nothing else can take focus) or when an app flashes its button. In hover
// only mode the taskbar windows stay hidden until the cursor reaches the bottom edge of their
// monitor; auto-hide then slides them in as usual, and they are hidden again a moment after the
// cursor left and no taskbar popup (Start, tray, calendar, quick settings) is open.
class TaskbarManager: public QObject {
	Q_OBJECT;

public:
	static TaskbarManager* instance();
	~TaskbarManager() override;
	Q_DISABLE_COPY_MOVE(TaskbarManager);

	[[nodiscard]] bool hoverOnly() const { return this->mHoverOnly; }
	void setHoverOnly(bool hoverOnly);

	// For the crash handler: shows the taskbars and restores Windows' auto-hide setting. Plain
	// Win32 on fixed state, safe to call from an exception filter.
	static void restoreForCrash();

signals:
	void hoverOnlyChanged();

private:
	explicit TaskbarManager(QObject* parent);

	struct Bar {
		HWND hwnd = nullptr;
		QRect monitor; // physical
		TaskbarEdge edge = TaskbarEdge::Bottom;
		int thickness = 0; // full thickness along the thin axis, even while auto-hidden
	};

	void enable();
	void disable();
	void findBars();
	void onCursorMoved(QPoint position);
	void onCheck();
	void reveal();
	void conceal();
	[[nodiscard]] bool atTrigger(QPoint position) const;
	[[nodiscard]] bool overBar(QPoint position) const;
	[[nodiscard]] static bool taskbarPopupActive();
	void watchExplorer();
	void unwatchExplorer();
	void onBarShown(HWND hwnd);
	static void CALLBACK onWinEvent(
	    HWINEVENTHOOK hook,
	    DWORD event,
	    HWND hwnd,
	    LONG idObject,
	    LONG idChild,
	    DWORD thread,
	    DWORD time
	);

	bool mHoverOnly = false;
	bool enabled = false;
	bool revealed = false;
	qint64 leftAt = 0;
	QList<Bar> bars;
	QTimer checkTimer;
	HWINEVENTHOOK showHook = nullptr;
	DWORD watchedPid = 0;
	qint64 burstStart = 0;
	int burstHides = 0;
};

///! The Windows taskbar.
/// Windows only. Set @@hoverOnly to keep the taskbar out of sight until the cursor touches the
/// bottom edge of the screen, like a compositor without one. Turns Windows' auto-hide on while
/// set (and back off afterwards if it was off), and puts everything back when the shell exits.
class Taskbar: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	/// Show the taskbar only while the cursor is on it or at the bottom edge of its screen.
	Q_PROPERTY(bool hoverOnly READ hoverOnly WRITE setHoverOnly NOTIFY hoverOnlyChanged);
	// clang-format on

public:
	explicit Taskbar(QObject* parent = nullptr);

	[[nodiscard]] bool hoverOnly() const;
	void setHoverOnly(bool hoverOnly);

signals:
	void hoverOnlyChanged();
};

} // namespace qs::windows
