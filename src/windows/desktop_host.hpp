#pragma once

#include <qt_windows.h>

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

// Where panels that are part of the desktop (wallpapers, desktop widgets) live: inside the
// explorer window that draws the wallpaper, behind the desktop icons. That is how wallpaper
// engines do it, and what keeps such panels out of Alt+Tab, Task View and the taskbar, and
// on screen through Show Desktop (Win+D) and Aero Peek, which a bottom-most top level window
// can't do.
//
// Explorer only creates that window when asked (message 0x052C to Progman), and the layout
// changed over time:
// - Windows 10 and 11 before 24H2: a top level WorkerW right behind the one holding the icons
//   view (SHELLDLL_DefView).
// - Windows 11 24H2 and later: the WorkerW is a child of Progman, below the icons view, which
//   moved into Progman itself.
// Without a WorkerW, Progman draws the wallpaper itself; panels then go into Progman right
// below the icons view.
//
// Child windows get none of the broadcasts top level windows get (TaskbarCreated, display
// changes), so a hidden top level window listens for them, and a WinEvent hook follows the
// parent's moves and destruction.
class DesktopHost: public QObject {
	Q_OBJECT;

public:
	static DesktopHost* instance();
	~DesktopHost() override;
	Q_DISABLE_COPY_MOVE(DesktopHost);

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);

	// The window desktop panels go into, or null: embedding is off, or explorer has no desktop
	// window right now (panels then stay bottom-most top level windows).
	[[nodiscard]] HWND parentWindow();
	// Sibling to place desktop panels right below, when the parent also holds the icons view.
	[[nodiscard]] HWND insertAfter() const { return this->mInsertAfter; }

	// Enabled and a desktop window was found.
	[[nodiscard]] bool active() const { return this->mEnabled && this->mParent != nullptr; }

signals:
	void enabledChanged();
	void activeChanged();
	// Desktop panels have to be re-parented (or put back as top level windows).
	void parentChanged();
	// The parent moved or resized (it spans the virtual screen): re-place the panels in it.
	void parentMoved();

private:
	explicit DesktopHost(QObject* parent);

	void ensureListener();
	void scheduleRefresh(int delayMs = 0);
	void refresh();
	void lookup();
	void installHook();
	void removeHook();

	static LRESULT CALLBACK listenerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
	static void CALLBACK eventProc(
	    HWINEVENTHOOK hook,
	    DWORD event,
	    HWND hwnd,
	    LONG idObject,
	    LONG idChild,
	    DWORD thread,
	    DWORD time
	);

	bool mEnabled = false;
	// The last lookup's result; only redone when something says the desktop changed.
	bool lookedUp = false;
	HWND mParent = nullptr;
	HWND mInsertAfter = nullptr;
	HWND listener = nullptr;
	HWINEVENTHOOK hook = nullptr;
	HWINEVENTHOOK moveHook = nullptr;
	DWORD hookThread = 0;
	bool movePending = false;
	QTimer refreshTimer;
};

///! The desktop layer.
/// Windows only. While @@enabled is set, panels on the `Background` and `Bottom` layers that
/// neither reserve screen space nor take keyboard focus (wallpapers, desktop widgets) become
/// part of the Windows desktop, behind the desktop icons, instead of separate windows: they
/// stay out of Alt+Tab, Task View and the taskbar and stay visible through Show Desktop.
///
/// The icons view covers the whole desktop, so these panels get no mouse input. If explorer
/// has no desktop window to put them in (or Windows refuses), they stay bottom-most windows.
class DesktopLayer: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	/// Put desktop panels behind the desktop icons. Off by default.
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	/// True while desktop panels are actually inside the desktop.
	Q_PROPERTY(bool active READ active NOTIFY activeChanged);
	// clang-format on

public:
	explicit DesktopLayer(QObject* parent = nullptr);

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool enabled);

	[[nodiscard]] bool active() const;

signals:
	void enabledChanged();
	void activeChanged();
};

} // namespace qs::windows
