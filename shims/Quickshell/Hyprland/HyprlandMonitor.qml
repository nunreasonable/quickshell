// Windows shim for Quickshell.Hyprland's HyprlandMonitor.
// Real backend: Windows virtual-desktop/monitor tracking (EnumDisplayMonitors
// plus whatever per-monitor virtual-desktop API ends up backing workspaces).
//
// Upstream (src/wayland/hyprland/ipc/monitor.hpp) is QML_UNCREATABLE and only
// ever handed out by the Hyprland singleton; instances here are likewise only
// created by Hyprland.qml (one per Quickshell.screens entry), never by ii.
import QtQml

QtObject {
    property int id: -1
    property string name: ""
    property string description: ""
    property int x: 0
    property int y: 0
    property int width: 0
    property int height: 0
    property real scale: 1.0
    property var lastIpcObject: ({})
    property QtObject activeWorkspace: null
    property bool focused: false
}
