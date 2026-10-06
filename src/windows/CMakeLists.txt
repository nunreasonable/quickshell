qt_add_library(quickshell-windows STATIC
	util.cpp
	appbar.cpp
	input_mask.cpp
	panel_window.cpp
	blur.cpp
	blur_shapes.cpp
	keyboard_hook.cpp
	hotkeys.cpp
	focus_grab.cpp
	virtual_desktops.cpp
	window_tracker.cpp
	tiling_layout.cpp
	tiling.cpp
	super_drag.cpp
	taskbar.cpp
	capture.cpp
	desktop_host.cpp
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
	system/gamma.cpp
	system/screenshot.cpp
	system/notification_settings.cpp
	system/network.cpp
	system/network_wifi.cpp
	system/network_wifi_item.cpp
	system/network_connectivity.cpp
	system/wallpaper.cpp
	system/fs_utils.cpp
	system/terminal_colors.cpp
	system/terminal_colors_math.cpp
	image/image_tools.cpp
	image/thumbnailer.cpp
	system/ocr.cpp
	system/ocr_backend.cpp
	system/ocr_worker.cpp
	system/screen_recorder.cpp
	recorder/recording.cpp
	recorder/region_capture.cpp
	recorder/mp4_writer.cpp
	recorder/loopback.cpp
	system/audio_visualizer.cpp
	visualizer/spectrum.cpp
	visualizer/loopback_source.cpp
)

qt_add_qml_module(quickshell-windows
	URI Quickshell.Windows
	VERSION 0.1
	DEPENDENCIES QtQuick
)

qs_add_module_deps_light(quickshell-windows Quickshell)

install_qml_module(quickshell-windows)

target_include_directories(quickshell-windows PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/system
	${CMAKE_CURRENT_SOURCE_DIR}/image
)

add_subdirectory(wayland)
add_subdirectory(services)
add_subdirectory(hyprland)
add_subdirectory(bluetooth)
add_subdirectory(third_party/material_color)

if (CRASH_HANDLER)
	add_subdirectory(crash)
endif()

add_library(quickshell-windows-init OBJECT init.cpp)

target_link_libraries(quickshell-windows PRIVATE
	Qt::Quick
	Qt::QuickPrivate
	quickshell-windows-services
	user32
	shell32
	dwmapi
	gdi32
	advapi32
	powrprof
	winmm
	dxva2
	pdh
	wbemuuid
	ole32
	oleaut32
	uuid
	windowsapp
	runtimeobject
	d3d11
	dxgi
	wlanapi
	iphlpapi
	CoreMessaging
	material-color
	mfplat
	mfreadwrite
	mfuuid
	delayimp
)
target_link_libraries(quickshell-windows-init PRIVATE Qt::Quick Qt::Qml)

qs_module_pch(quickshell-windows SET large)

target_link_libraries(quickshell PRIVATE quickshell-windowsplugin quickshell-windows-init)

foreach(exe quickshell quickshell-gui)
	if (TARGET ${exe})
		target_link_options(${exe} PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)
	endif()
endforeach()
