// Quickshell.Hyprland's GlobalShortcut on Windows.
// Registers with the native Quickshell.Windows Hotkeys singleton, which owns the keys
// (keybinds.json: RegisterHotKey + a low level keyboard hook) and signals every shortcut whose
// appid and name match a bind's `"action": "global", "name": "<appid>:<name>"` (appid defaults
// to "quickshell", as in Hyprland's `global, quickshell:<name>`).
//
// NOTE: upstream's C++ type has both a `pressed` *property* (poll-only state)
// and a `pressed()` *signal* -- legal there because Qt's meta-object system
// keeps properties and signals in separate tables. Hand-authored QML shares
// one namespace for both, so a `pressed` property here would shadow the
// `pressed()` signal and break `onPressed:` handlers (confirmed empirically).
// ii only ever uses `onPressed`/`onReleased`, so the polling property is
// dropped here; everything else matches upstream.
import QtQml
import Quickshell.Windows

QtObject {
    id: root

    property string appid: "quickshell"
    property string name: ""
    property string description: ""
    property string triggerDescription: ""

    signal pressed()
    signal released()

    Component.onCompleted: Hotkeys.registerShortcut(root)
    Component.onDestruction: Hotkeys.unregisterShortcut(root)
}
