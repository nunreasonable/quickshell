# Native Windows backend for Quickshell.Services.UPower (replaces the QML shim of the same URI).
# GetSystemPowerStatus + RegisterPowerSettingNotification for the aggregate battery, and the
# powrprof overlay scheme APIs for power profiles. No DBus involved.

qt_add_library(quickshell-windows-upower STATIC
	core.cpp
	device.cpp
	powerprofiles.cpp
)

qt_add_qml_module(quickshell-windows-upower
	URI Quickshell.Services.UPower
	VERSION 0.1
	DEPENDENCIES QtQuick
)

qs_add_module_deps_light(quickshell-windows-upower Quickshell)

install_qml_module(quickshell-windows-upower)

target_link_libraries(quickshell-windows-upower PRIVATE Qt::Quick quickshell-windows-services powrprof)

qs_module_pch(quickshell-windows-upower SET large)

target_link_libraries(quickshell PRIVATE quickshell-windows-upowerplugin)
