// Windows shim for Quickshell.Services.Polkit's Identity.
// Real backend: whatever identity model eventually backs elevation on
// Windows (there is no polkit there; UAC works very differently). Not used
// by ii today beyond AuthFlow.identities' element type; kept for API
// completeness.
import QtQml

QtObject {
    property string id: ""
    property string name: ""
    property string displayName: ""
    property bool isGroup: false
}
