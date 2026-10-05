#pragma once

#include <functional>

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
// The window lives on its own thread with its own message loop: apps block in Shell_NotifyIcon
// until it answers, so it must never wait on the Qt GUI thread. If the process exits or crashes
// the window goes away with it and FindWindow finds explorer's again.
class TrayHook {
public:
	// The sink is called on the hook thread.
	static void start(TrayIconSink sink);
	static void stop();
};

// Reads the icons explorer already shows through its ITrayNotify interface, on a short lived
// thread of its own. Calls the sink there.
void seedFromExplorer(TrayIconSink sink);

} // namespace qs::windows::services::systray
