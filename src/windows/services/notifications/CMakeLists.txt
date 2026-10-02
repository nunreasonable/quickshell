# Native replacement for the Quickshell.Services.Notifications shim, with the same QML API ii
# uses. There is no D-Bus server on Windows: notifications come from the shell itself
# (NotificationServer.notifySend, the stand-in for notify-send) and from the Windows notification
# center, read through UserNotificationListener (C++/WinRT) on a dedicated MTA worker thread
# (toast_worker.*; toast_mirror.* is its GUI-thread owner, see docs/AGENTS.md for why).

qt_add_library(quickshell-windows-notifications STATIC
	notification.cpp
	notify_send.cpp
	server.cpp
	toast_mirror.cpp
	toast_worker.cpp
	qml.cpp
)

qt_add_qml_module(quickshell-windows-notifications
	URI Quickshell.Services.Notifications
	VERSION 0.1
	DEPENDENCIES QtQml
)

qs_add_module_deps_light(quickshell-windows-notifications Quickshell)

install_qml_module(quickshell-windows-notifications)

# quickshell-windows: the window tracker (raising a mirrored toast's app) and the Apps folder
# backend (AUMID -> DesktopEntries id, launching). windowsapp/runtimeobject: WinRT activation for
# Windows.UI.Notifications.Management from a classic desktop app. Qt::Gui: QImage, to re-save
# app logos as PNG.
target_link_libraries(quickshell-windows-notifications PRIVATE
	Qt::Quick Qt::Gui quickshell-core quickshell-windows windowsapp runtimeobject
)

qs_module_pch(quickshell-windows-notifications SET large)

target_link_libraries(quickshell PRIVATE quickshell-windows-notificationsplugin)
