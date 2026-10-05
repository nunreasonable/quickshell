# Native replacement for the Quickshell.Services.SystemTray shim, with the QML API of the Linux
# StatusNotifierItem module. Icons come from Shell_NotifyIcon traffic: a stand-in Shell_TrayWnd
# on its own thread receives it and passes everything on to explorer's (hook.*), and explorer's
# ITrayNotify lists the icons that were there before us (seed.cpp).

qt_add_library(quickshell-windows-systray STATIC
	hook.cpp
	seed.cpp
	host.cpp
	item.cpp
	qml.cpp
)

qt_add_qml_module(quickshell-windows-systray
	URI Quickshell.Services.SystemTray
	VERSION 0.1
	DEPENDENCIES QtQml
)

qs_add_module_deps_light(quickshell-windows-systray Quickshell)

install_qml_module(quickshell-windows-systray)

# quickshell-windows: util (finding explorer's taskbar) and the exe icon fallback (appicon).
# version: executable descriptions for item titles.
target_link_libraries(quickshell-windows-systray PRIVATE
	Qt::Quick Qt::Gui quickshell-core quickshell-windows user32 shell32 ole32 version
)

qs_module_pch(quickshell-windows-systray SET large)

target_link_libraries(quickshell PRIVATE quickshell-windows-systrayplugin)
