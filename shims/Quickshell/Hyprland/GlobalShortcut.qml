// Windows shim for Quickshell.Hyprland's GlobalShortcut.
// Real backend: RegisterHotKey / a low-level keyboard hook, keyed by the
// appid+name pair like hyprland_global_shortcuts_v1.
//
// NOTE: upstream's C++ type has both a `pressed` *property* (poll-only state)
// and a `pressed()` *signal* -- legal there because Qt's meta-object system
// keeps properties and signals in separate tables. Hand-authored QML shares
// one namespace for both, so a `pressed` property here would shadow the
// `pressed()` signal and break `onPressed:` handlers (confirmed empirically).
// ii only ever uses `onPressed`/`onReleased`, so the polling property is
// dropped here; everything else matches upstream.
import QtQml

QtObject {
    property string appid: "quickshell"
    property string name: ""
    property string description: ""
    property string triggerDescription: ""

    signal pressed()
    signal released()
}
