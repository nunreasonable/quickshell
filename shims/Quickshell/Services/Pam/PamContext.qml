// Windows shim for Quickshell.Services.Pam's PamContext.
// Real backend: a Windows credential/authentication API (e.g. CredUI or
// LogonUser) standing in for a PAM conversation.
//
// There's no PAM on Windows, so `start()` can't actually authenticate
// anyone yet: it always finishes asynchronously with `completed(PamResult.
// Error)`, matching the documented async nature of a real conversation
// without claiming a success or failure it can't back up. ii's lock screen
// already has an else-branch for non-Success results, so this degrades
// safely (shows the normal "auth failed" UI rather than unlocking).
import QtQml

QtObject {
    id: root

    property bool active: false
    property string config: "login"
    property string configDirectory: "/etc/pam.d"
    property string user: ""
    readonly property string message: ""
    readonly property bool messageIsError: false
    readonly property bool responseRequired: false
    readonly property bool responseVisible: false

    signal completed(int result)
    signal error(int error)
    signal pamMessage()

    property Timer _timer: Timer {
        interval: 50
        onTriggered: {
            root.active = false;
            root.completed(PamResult.Error);
        }
    }

    function start() {
        if (root.active) return false;
        root.active = true;
        root._timer.start();
        return true;
    }

    function abort() {
        root.active = false;
        root._timer.stop();
    }

    function respond(response) {
        console.info("[shim] PamContext.respond (ignored, no backend yet)");
    }
}
