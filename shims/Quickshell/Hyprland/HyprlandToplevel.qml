// Windows shim for Quickshell.Hyprland's HyprlandToplevel.
// Real backend: Windows top-level window tracking (EnumWindows / shell hooks).
//
// Upstream (src/wayland/hyprland/ipc/hyprland_toplevel.hpp) is QML_UNCREATABLE
// and is also usable as an attached property on a Quickshell.Wayland Toplevel
// (`toplevel.HyprlandToplevel`). ii only ever reads that attached form, and
// only on objects that come from Quickshell.Wayland's ToplevelManager, which
// is a separate Linux-only module outside this shim's scope (and isn't
// provided on Windows either) -- so that attachment is intentionally not
// replicated here; see the final report for details. This type still exists
// so the Hyprland module's own `activeToplevel` property and
// `HyprlandWorkspace.toplevels` list have a sane (always-empty, in Phase 2)
// element type.
import QtQml

QtObject {
    property string address: ""
    property QtObject handle: null
    property var wayland: null
    property string title: ""
    property bool activated: false
    property bool urgent: false
    property var lastIpcObject: ({})
    property QtObject workspace: null
    property QtObject monitor: null
}
