#pragma once

#include <functional>
#include <vector>

#include <qt_windows.h>

#include <qimage.h>
#include <qstring.h>
#include <quuid.h>

namespace qs::windows::services::systray {

// One Shell_NotifyIcon call, decoded on the hook thread (icons are copied there, while the app
// still waits on its call and its HICON is valid).
struct TrayIconMessage {
	DWORD message = 0; // NIM_*
	HWND hwnd = nullptr;
	UINT uid = 0;
	UINT flags = 0; // NIF_*
	UINT callbackMessage = 0;
	QImage icon;
	QString tip;
	DWORD state = 0;
	DWORD stateMask = 0;
	UINT version = 0; // NIM_SETVERSION only
	QUuid guid;

	// From explorer's list of icons that were already there (seed.cpp) rather than from a
	// Shell_NotifyIcon call: no callback message or version is known for these.
	bool seeded = false;
	QString exePath;
};

using TrayIconSink = std::function<void(TrayIconMessage)>;

// Receives the notification area traffic while explorer keeps running and keeps its own tray:
// a hidden Shell_TrayWnd class window of ours that stays above explorer's in z-order, so the
// FindWindow in shell32's Shell_NotifyIcon (and SHAppBarMessage) reaches it first. Every
// message is passed on to explorer's window synchronously and explorer's answer goes back to
// the caller, so the taskbar works the same with or without us. At start it sends TaskbarCreated
// to the other processes' windows, which makes apps add their icons again (the documented
// reaction to an explorer restart): that's the only way to learn the callback message of icons
// registered before us.
//
// Tools that look for explorer's taskbar by class get this window too. It carries explorer's
// window properties, rect and TrayNotifyWnd child, and a title of its own so lookups that also
// ask for explorer's empty title skip it (shell32's doesn't, checked at start).
//
// The window lives on its own thread with its own message loop: apps block in Shell_NotifyIcon
// until it answers, so it must never wait on the Qt GUI thread. If the process exits or crashes
// the window goes away with it and FindWindow finds explorer's again.
class TrayHook {
public:
	// Both callbacks are called on the hook thread. missedTraffic is called when explorer's
	// window was found above ours: whatever apps sent in the meantime went to explorer only.
	//
	// Brief mode keeps the window in front only for a few seconds after each TaskbarCreated
	// (ours, explorer's, announceTo()) and right behind explorer's otherwise, for tools that
	// mistake it for the taskbar. Icon changes then come from reading explorer's list.
	static void start(TrayIconSink sink, std::function<void()> missedTraffic, bool brief);
	static void stop();

	// Sends TaskbarCreated to just these windows (icon owners), from the hook thread once ours
	// is in front again, so their answer comes through the hook. Reaches message-only windows,
	// which the broadcast doesn't. Callable from any thread.
	static void announceTo(std::vector<HWND> owners);
};

// Reads the icons explorer already shows through its ITrayNotify interface, on a short lived
// thread of its own. Calls the sink there for each icon, then done (whether explorer answered).
void seedFromExplorer(TrayIconSink sink, std::function<void(bool ok)> done);

// What explorer's own notification area knows about an icon and ITrayNotify leaves out.
struct ExplorerIconData {
	HWND hwnd = nullptr;
	UINT uid = 0;
	UINT callbackMessage = 0;
	UINT version = 0;
};

// Reads the callback message and version of the icons from explorer's tray toolbars (the
// Windows 10 notification area and its overflow window; Windows 11 before its new tray).
// Empty where explorer has no such toolbars. Blocks on explorer: call off the GUI thread.
std::vector<ExplorerIconData> readExplorerToolbars();

} // namespace qs::windows::services::systray
