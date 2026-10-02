# Windows backend: PanelWindow on top of Win32 (AppBars for exclusive zones, DWM attributes,
# a low level mouse hook for input masks), global shortcuts (RegisterHotKey + a low level
# keyboard hook) and focus grabs, the window / virtual desktop tracker, the
# Windows.Graphics.Capture engine (capture.cpp) behind ScreencopyView and Screenshot, stand-in
# Quickshell.Wayland and Quickshell.Hyprland modules built on it, small native singletons
# (system/) ii's services call into instead of spawning Linux CLIs, and native service modules
# (services/) with the URIs of the Linux ones: Pipewire on Core Audio, UPower, Mpris on GSMTC,
# plus Quickshell.Bluetooth on WinRT (bluetooth/).

qt_add_library(quickshell-windows STATIC
	util.cpp
	appbar.cpp
	input_mask.cpp
	panel_window.cpp
	keyboard_hook.cpp
	hotkeys.cpp
	focus_grab.cpp
	virtual_desktops.cpp
	window_tracker.cpp
	capture.cpp
	desktopentry_backend.cpp
	appicon.cpp
	system/session.cpp
	system/system_stats.cpp
	system/brightness.cpp
	system/keyboard.cpp
	system/clipboard.cpp
	system/credentials.cpp
	system/input.cpp
	system/night_light.cpp
	system/screenshot.cpp
)

qt_add_qml_module(quickshell-windows
	URI Quickshell.Windows
	VERSION 0.1
	DEPENDENCIES QtQuick
)

qs_add_module_deps_light(quickshell-windows Quickshell)

install_qml_module(quickshell-windows)

# qmltyperegistrar emits "#if __has_include(<foo.hpp>)" using just the header's basename, so
# system/ (where the new native singletons' headers live) needs its own spot on the include path.
target_include_directories(quickshell-windows PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/system)

add_subdirectory(wayland)
add_subdirectory(services)
add_subdirectory(hyprland)
add_subdirectory(bluetooth)

add_library(quickshell-windows-init OBJECT init.cpp)

target_link_libraries(quickshell-windows PRIVATE
	Qt::Quick
	quickshell-windows-services
	user32
	shell32
	dwmapi
	gdi32
	advapi32
	powrprof
	dxva2
	pdh
	wbemuuid
	ole32
	oleaut32
	uuid
	# capture.cpp: WinRT activation for Windows.Graphics.Capture and the D3D11 device it runs on.
	windowsapp
	runtimeobject
	d3d11
	dxgi
)
target_link_libraries(quickshell-windows-init PRIVATE Qt::Quick Qt::Qml)

qs_module_pch(quickshell-windows SET large)

target_link_libraries(quickshell PRIVATE quickshell-windowsplugin quickshell-windows-init)
