#pragma once

#include <qt_windows.h>

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstringlist.h>
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
// Desktop widgets that take the mouse can't live back there: the icons view (SHELLDLL_DefView)
// covers the whole desktop and takes every click. Those go right above the icons view, into the
// window holding it (Progman, or the top level WorkerW before 24H2), with their input mask as
// their window region so the icons get the clicks everywhere else. Inside the icons view itself
// would follow it around, but how explorer draws it changed across builds (on 24H2 it may be
// layered, no place to rely on for DirectX content), while children of Progman are where the
// wallpaper panels are known to render. Explorer moves the icons view between Progman and a
// WorkerW when it rebuilds the desktop, which is watched for.
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
	// The window holding the icons view, where desktop panels that take input go (above the
	// icons view), or null.
	[[nodiscard]] HWND iconsHost();

	// Namespaces of desktop panels that go above the icons instead of behind them.
	[[nodiscard]] QStringList aboveIcons() const { return this->mAboveIcons; }
	void setAboveIcons(const QStringList& namespaces);

	// Enabled and a desktop window was found.
	[[nodiscard]] bool active() const { return this->mEnabled && this->mParent != nullptr; }

signals:
	void enabledChanged();
	void activeChanged();
	// Desktop panels have to be re-parented (or put back as top level windows).
	void parentChanged();
	// The parent or the icons view moved or resized (they span the virtual screen): re-place the
	// panels in them.
	void parentMoved();
	void aboveIconsChanged();
	// Explorer created, showed or restacked something next to the icons view: panels above the
	// icons make sure they still are.
	void iconsRestacked();

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
	HWND mIconsView = nullptr;
	HWND mIconsHost = nullptr;
	QStringList mAboveIcons;
	HWND listener = nullptr;
	HWINEVENTHOOK hook = nullptr;
	HWINEVENTHOOK moveHook = nullptr;
	DWORD hookThread = 0;
	bool movePending = false;
	bool restackPending = false;
	QTimer refreshTimer;
};

///! The desktop layer.
/// Windows only. While @@enabled is set, panels on the `Background` and `Bottom` layers that
/// neither reserve screen space nor take keyboard focus (wallpapers, desktop widgets) become
/// part of the Windows desktop, behind the desktop icons, instead of separate windows: they
/// stay out of Alt+Tab, Task View and the taskbar and stay visible through Show Desktop.
///
/// The icons view covers the whole desktop, so panels behind it get no mouse input. Desktop
/// widgets that have to be clicked or dragged go above the icons instead (see @@aboveIcons),
/// still inside the desktop. If explorer has no desktop window to put them in (or Windows
/// refuses), they stay bottom-most windows.
class DesktopLayer: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	/// Put desktop panels behind the desktop icons. Off by default.
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	/// True while desktop panels are actually inside the desktop.
	Q_PROPERTY(bool active READ active NOTIFY activeChanged);
	/// Namespaces (`WlrLayershell.namespace`) of desktop panels that go above the desktop icons
	/// instead of behind them, so they get the mouse. Such a panel only takes input inside its
	/// @@Quickshell.QsWindow.mask (the icons get the rest of the desktop) and is only drawn
	/// there, so the mask should cover everything it draws, shadows included. Without a mask it
	/// covers the icons. Still part of the desktop: below every application window and on
	/// screen through Show Desktop.
	Q_PROPERTY(QStringList aboveIcons READ aboveIcons WRITE setAboveIcons NOTIFY aboveIconsChanged);
	// clang-format on

public:
	explicit DesktopLayer(QObject* parent = nullptr);

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool enabled);

	[[nodiscard]] bool active() const;

	[[nodiscard]] QStringList aboveIcons() const;
	void setAboveIcons(const QStringList& namespaces);

signals:
	void enabledChanged();
	void activeChanged();
	void aboveIconsChanged();
};

} // namespace qs::windows
