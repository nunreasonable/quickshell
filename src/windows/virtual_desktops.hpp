#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtmetamacros.h>
#include <qtypes.h>

struct IVirtualDesktopManager;

namespace qs::windows {

// Blocks in RegNotifyChangeKeyValue on its own thread so the GUI thread never has to.
class RegistryWatcher: public QThread {
	Q_OBJECT;

public:
	explicit RegistryWatcher(QList<QString> subkeys, QObject* parent = nullptr);
	~RegistryWatcher() override;
	Q_DISABLE_COPY_MOVE(RegistryWatcher);

	void stop();

signals:
	void changed();

protected:
	void run() override;

private:
	QList<QString> subkeys;
	HANDLE stopEvent = nullptr;
};

///! Virtual desktops as the shell exposes them.
/// The ordered desktop list, the current desktop and the desktop names come from the registry,
/// which explorer keeps up to date; per window membership from the public IVirtualDesktopManager.
/// Switching desktops and moving other processes' windows has no public API, so
/// VirtualDesktopAccessor.dll (Ciantic, MIT; see docs) is loaded from next to the executable when
/// present, falling back to injecting the Ctrl+Win shortcuts.
class VirtualDesktops: public QObject {
	Q_OBJECT;

public:
	struct Desktop {
		GUID id {};
		QString name; // user given name, empty when unnamed
	};

	static VirtualDesktops* instance();

	[[nodiscard]] const QList<Desktop>& desktops() const { return this->mDesktops; }
	[[nodiscard]] qsizetype count() const { return this->mDesktops.length(); }
	[[nodiscard]] qsizetype currentIndex() const { return this->mCurrent; }
	[[nodiscard]] qsizetype indexOf(const GUID& id) const;
	[[nodiscard]] bool accessorLoaded() const { return this->accessor.loaded; }

	// Desktop of a window: GUID_NULL when unknown or when the window is shown on every desktop.
	[[nodiscard]] GUID windowDesktopId(HWND hwnd) const;
	[[nodiscard]] qsizetype windowDesktopIndex(HWND hwnd) const;
	[[nodiscard]] bool isWindowOnCurrent(HWND hwnd) const;

	bool switchTo(qsizetype index);
	// Creates desktops until there are at least `count`.
	bool ensureCount(qsizetype count);
	bool moveWindow(HWND hwnd, qsizetype index);
	bool pinWindow(HWND hwnd, bool pinned);
	[[nodiscard]] bool isWindowPinned(HWND hwnd) const;

	// Re-reads the registry now. Cheap, so it is also called on events that usually follow a
	// desktop switch (foreground changes, cloaking) in case the registry notification is late.
	void refresh();

	static QString guidToString(const GUID& guid);

signals:
	void desktopsChanged();
	void currentChanged();

private:
	explicit VirtualDesktops();
	~VirtualDesktops() override;
	Q_DISABLE_COPY_MOVE(VirtualDesktops);

	void loadAccessor();
	void installListener();
	bool readRegistry(QList<GUID>& ids, GUID& current) const;
	[[nodiscard]] QString readDesktopName(const GUID& id) const;
	bool waitForCount(qsizetype count);
	void sendShortcut(WORD key, int times) const;

	static LRESULT CALLBACK listenerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	QList<Desktop> mDesktops;
	qsizetype mCurrent = -1;
	GUID mCurrentId {};
	DWORD sessionId = 0;

	IVirtualDesktopManager* manager = nullptr;
	RegistryWatcher* watcher = nullptr;
	HWND listener = nullptr;

	struct Accessor {
		HMODULE module = nullptr;
		bool loaded = false;
		int (*getCurrentDesktopNumber)() = nullptr;
		int (*getDesktopCount)() = nullptr;
		GUID (*getDesktopIdByNumber)(int) = nullptr;
		int (*getWindowDesktopNumber)(HWND) = nullptr;
		int (*goToDesktopNumber)(int) = nullptr;
		int (*moveWindowToDesktopNumber)(HWND, int) = nullptr;
		int (*isWindowOnCurrentVirtualDesktop)(HWND) = nullptr;
		int (*createDesktop)() = nullptr;
		int (*registerPostMessageHook)(HWND, UINT) = nullptr;
		int (*unregisterPostMessageHook)(HWND) = nullptr;
		int (*isPinnedWindow)(HWND) = nullptr;
		int (*pinWindow)(HWND) = nullptr;
		int (*unPinWindow)(HWND) = nullptr;
	} accessor;
};

} // namespace qs::windows
