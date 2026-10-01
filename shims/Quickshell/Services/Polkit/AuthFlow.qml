// Windows shim for Quickshell.Services.Polkit's AuthFlow.
// Real backend: whatever elevation flow eventually replaces this on Windows
// (UAC consent/credential prompts work very differently from polkit).
//
// Upstream is QML_UNCREATABLE and only ever handed out via
// PolkitAgent.flow, which is always null in this phase (no authentication
// requests are ever generated), so no instance of this type exists at
// runtime today, but it's registered for API completeness.
import QtQml

QtObject {
    id: root

    property string message: ""
    property string iconName: ""
    property string actionId: ""
    property string cookie: ""
    property var identities: []
    property QtObject selectedIdentity: null
    property bool isResponseRequired: false
    property string inputPrompt: ""
    property bool responseVisible: false
    property string supplementaryMessage: ""
    property bool supplementaryIsError: false
    property bool isCompleted: false
    property bool isSuccessful: false
    property bool isCancelled: false
    property bool failed: false

    signal authenticationSucceeded()
    signal authenticationFailed()
    signal authenticationRequestCancelled()

    function submit(value) {
        console.info("[shim] AuthFlow.submit (ignored, no backend yet)");
    }

    function cancelAuthenticationRequest() {
        root.isCancelled = true;
        root.authenticationRequestCancelled();
    }
}
