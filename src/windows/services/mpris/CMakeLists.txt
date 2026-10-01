# Native replacement for the Quickshell.Services.Mpris shim: GlobalSystemMediaTransportControls
# SessionManager (Windows.Media.Control, C++/WinRT) standing in for MPRIS, with the same QML API
# ii uses. All WinRT calls happen on a dedicated MTA worker thread (gsmtc_worker.*);
# gsmtc_backend.* is the GUI-thread facade that owns that thread and relays commands/snapshots
# across it (see the comments there and in docs/AGENTS.md for why).

qt_add_library(quickshell-windows-mpris STATIC
	player.cpp
	mpris.cpp
	gsmtc_backend.cpp
	gsmtc_worker.cpp
	art_cache.cpp
)

qt_add_qml_module(quickshell-windows-mpris
	URI Quickshell.Services.Mpris
	VERSION 0.1
	DEPENDENCIES QtQml
)

qs_add_module_deps_light(quickshell-windows-mpris Quickshell)

install_qml_module(quickshell-windows-mpris)

# windowsapp/runtimeobject: WinRT activation (RoGetActivationFactory, hstring helpers) for
# consuming Windows.Media.Control from a classic desktop app. Qt::Gui: QImage, used to decode and
# re-save album art thumbnails as PNG regardless of their original encoding.
target_link_libraries(quickshell-windows-mpris PRIVATE Qt::Quick Qt::Gui windowsapp runtimeobject)

qs_module_pch(quickshell-windows-mpris SET large)

target_link_libraries(quickshell PRIVATE quickshell-windows-mprisplugin)
