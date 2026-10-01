# Stand-in for the Quickshell.Wayland module, which is not built on Windows. It carries the
# WlrLayershell attached object so configurations using it keep working, the IdleInhibitor, and
# ToplevelManager / Toplevel backed by the Windows window tracker. The remaining types of the
# real module (ScreencopyView, session lock) are QML stubs until something backs them.

qt_add_library(quickshell-windows-wayland STATIC
	wlr_layershell.cpp
	idle_inhibitor.cpp
	toplevel.cpp
)

qt_add_qml_module(quickshell-windows-wayland
	URI Quickshell.Wayland
	VERSION 0.1
	DEPENDENCIES QtQuick
	QML_FILES
		qml/ScreencopyView.qml
		qml/WlSessionLock.qml
		qml/WlSessionLockSurface.qml
)

qs_add_module_deps_light(quickshell-windows-wayland Quickshell Quickshell.Windows)

install_qml_module(quickshell-windows-wayland)

target_link_libraries(quickshell-windows-wayland PRIVATE Qt::Quick quickshell-core quickshell-windows powrprof)

qs_module_pch(quickshell-windows-wayland SET large)

target_link_libraries(quickshell PRIVATE quickshell-windows-waylandplugin)
