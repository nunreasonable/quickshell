# Windows stand-in for the Quickshell.Hyprland module: the Hyprland singleton, monitors
# (screens), workspaces (virtual desktops) and toplevels, all backed by the window tracker.
# GlobalShortcut and HyprlandFocusGrab are still QML stubs (qml/) until they get backends.

qt_add_library(quickshell-windows-hyprland STATIC
	connection.cpp
	dispatcher.cpp
	monitor.cpp
	workspace.cpp
	hyprland_toplevel.cpp
	qml.cpp
)

qt_add_qml_module(quickshell-windows-hyprland
	URI Quickshell.Hyprland
	VERSION 0.1
	DEPENDENCIES QtQuick
	QML_FILES
		qml/GlobalShortcut.qml
		qml/HyprlandFocusGrab.qml
)

qs_add_module_deps_light(quickshell-windows-hyprland Quickshell Quickshell.Wayland)

install_qml_module(quickshell-windows-hyprland)

target_link_libraries(quickshell-windows-hyprland PRIVATE
	Qt::Quick quickshell-core quickshell-windows quickshell-windows-wayland user32 shell32
)

qs_module_pch(quickshell-windows-hyprland SET large)

target_link_libraries(quickshell PRIVATE quickshell-windows-hyprlandplugin)
