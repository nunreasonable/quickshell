// Windows shim for Quickshell.Services.Polkit's PolkitAgent.
// Real backend: there is no polkit on Windows; a future backend would need
// to either bridge to UAC elevation (a fundamentally different model: a
// separate elevated process launch, not an in-place auth conversation) or
// simply never register. For now this stays permanently idle: never
// registers, never activates, `flow` stays null -- ii's polkit prompt UI
// (FullscreenPolkitWindow etc.) is gated on `isActive`, so it just never
// shows, which is the safe inert behaviour for this phase.
import QtQml

QtObject {
    property string path: "/org/quickshell/Polkit"
    readonly property bool isRegistered: false
    readonly property bool isActive: false
    readonly property QtObject flow: null

    signal authenticationRequestStarted()
}
