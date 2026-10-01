# Stand-in for the Quickshell.Wayland module, which is not built on Windows. It carries the
# WlrLayershell attached object so configurations using it keep working. Other types of the
# real module (ToplevelManager, ScreencopyView, ...) can be added to this module as needed.

qt_add_library(quickshell-windows-wayland STATIC
	wlr_layershell.cpp
	idle_inhibitor.cpp
)

set_source_files_properties(qml/ToplevelManager.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(quickshell-windows-wayland
	URI Quickshell.Wayland
	VERSION 0.1
	DEPENDENCIES QtQuick
	QML_FILES
		qml/ToplevelManager.qml
		qml/Toplevel.qml
		qml/ScreencopyView.qml
		qml/WlSessionLock.qml
		qml/WlSessionLockSurface.qml
)

qs_add_module_deps_light(quickshell-windows-wayland Quickshell Quickshell.Windows)

install_qml_module(quickshell-windows-wayland)

target_link_libraries(quickshell-windows-wayland PRIVATE Qt::Quick quickshell-windows powrprof)

qs_module_pch(quickshell-windows-wayland SET large)

target_link_libraries(quickshell PRIVATE quickshell-windows-waylandplugin)
