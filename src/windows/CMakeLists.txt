# Windows backend.
#
# Only the plugin skeleton exists for now. The actual window backend (PanelWindow and friends
# on top of Win32/DWM) is a later task and will live in this directory as a QML module.

add_library(quickshell-windows-init OBJECT init.cpp)
target_link_libraries(quickshell-windows-init PRIVATE Qt::Qml)

target_link_libraries(quickshell PRIVATE quickshell-windows-init)
