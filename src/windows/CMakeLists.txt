# Windows backend: PanelWindow on top of Win32 (AppBars for exclusive zones, DWM attributes,
# a low level mouse hook for input masks) plus a stand-in Quickshell.Wayland module and a
# native Quickshell.Services.Mpris module backed by GSMTC.

qt_add_library(quickshell-windows STATIC
	util.cpp
	appbar.cpp
	input_mask.cpp
	panel_window.cpp
)

qt_add_qml_module(quickshell-windows
	URI Quickshell.Windows
	VERSION 0.1
	DEPENDENCIES QtQuick
)

qs_add_module_deps_light(quickshell-windows Quickshell)

install_qml_module(quickshell-windows)

add_subdirectory(wayland)
add_subdirectory(services/mpris)

add_library(quickshell-windows-init OBJECT init.cpp)

target_link_libraries(quickshell-windows PRIVATE Qt::Quick user32 shell32 dwmapi)
target_link_libraries(quickshell-windows-init PRIVATE Qt::Quick Qt::Qml)

qs_module_pch(quickshell-windows SET large)

target_link_libraries(quickshell PRIVATE quickshell-windowsplugin quickshell-windows-init)
