// Windows shim for Quickshell.Hyprland's HyprlandFocusGrab.
// Real backend: a Win32 focus/activation watcher (e.g. a WH_CBT/WH_MOUSE
// hook, or polling GetForegroundWindow) that emits `cleared()` once input
// moves outside the listed windows, mirroring hyprland_focus_grab_v1.
//
// This stub tracks `active`/`windows` as plain properties but never performs
// a real grab and never emits `cleared()` on its own -- popups that rely on
// click-outside-to-dismiss simply won't auto-dismiss yet in this phase.
import QtQml

QtObject {
    property bool active: false
    property var windows: []

    signal cleared()
}
