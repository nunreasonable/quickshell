// Windows shim for Quickshell.Hyprland's HyprlandWorkspace.
// Real backend: Windows virtual-desktop tracking (IVirtualDesktopManager et al).
//
// Upstream (src/wayland/hyprland/ipc/workspace.hpp) is QML_UNCREATABLE and only
// ever handed out by the Hyprland singleton; instances here are likewise only
// created by Hyprland.qml, never by ii.
import QtQml
import "../../_common" as Common

QtObject {
    id: root

    property int id: -1
    property string name: ""
    property bool active: false
    property bool focused: false
    property bool urgent: false
    property bool hasFullscreen: false
    property var lastIpcObject: ({})
    property QtObject monitor: null
    readonly property Common.ObjectModel toplevels: Common.ObjectModel {}

    /// Mirrors upstream's Q_INVOKABLE activate(), equivalent to
    /// `Hyprland.dispatch("workspace " + name)`.
    function activate() {
        console.info("[shim] HyprlandWorkspace.activate", root.name);
    }
}
