# Windows backend: PanelWindow on top of Win32 (AppBars for exclusive zones, DWM attributes,
# a low level mouse hook for input masks), global shortcuts (RegisterHotKey + a low level
# keyboard hook), focus grabs, plus a stand-in Quickshell.Wayland module.

qt_add_library(quickshell-windows STATIC
	util.cpp
	appbar.cpp
	input_mask.cpp
	panel_window.cpp
	keyboard_hook.cpp
	hotkeys.cpp
	focus_grab.cpp
)

qt_add_qml_module(quickshell-windows
	URI Quickshell.Windows
	VERSION 0.1
	DEPENDENCIES QtQuick
)

qs_add_module_deps_light(quickshell-windows Quickshell)

install_qml_module(quickshell-windows)

add_subdirectory(wayland)

add_library(quickshell-windows-init OBJECT init.cpp)

target_link_libraries(quickshell-windows PRIVATE Qt::Quick user32 shell32 dwmapi advapi32)
target_link_libraries(quickshell-windows-init PRIVATE Qt::Quick Qt::Qml)

qs_module_pch(quickshell-windows SET large)

target_link_libraries(quickshell PRIVATE quickshell-windowsplugin quickshell-windows-init)
