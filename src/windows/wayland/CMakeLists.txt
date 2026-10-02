# Stand-in for the Quickshell.Wayland module, which is not built on Windows. It carries the
# WlrLayershell attached object so configurations using it keep working, the IdleInhibitor,
# ToplevelManager / Toplevel backed by the Windows window tracker, and ScreencopyView on
# Windows.Graphics.Capture (frames reach the D3D11 scene graph as shared textures). The session
# lock types are still QML stubs until something backs them.

qt_add_library(quickshell-windows-wayland STATIC
	wlr_layershell.cpp
	idle_inhibitor.cpp
	toplevel.cpp
	screencopy_view.cpp
)

qt_add_qml_module(quickshell-windows-wayland
	URI Quickshell.Wayland
	VERSION 0.1
	DEPENDENCIES QtQuick
	QML_FILES
		qml/WlSessionLock.qml
		qml/WlSessionLockSurface.qml
)

qs_add_module_deps_light(quickshell-windows-wayland Quickshell Quickshell.Windows)

install_qml_module(quickshell-windows-wayland)

target_link_libraries(quickshell-windows-wayland PRIVATE Qt::Quick quickshell-core quickshell-windows powrprof d3d11)

qs_module_pch(quickshell-windows-wayland SET large)

target_link_libraries(quickshell PRIVATE quickshell-windows-waylandplugin)
