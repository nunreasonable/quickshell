// Windows shim for Quickshell.Services.Pipewire's PwLink.
// Real backend: Core Audio. Not used by ii today (it uses PwLinkGroup
// instead); kept for API completeness. Pipewire.links is always empty in
// this shim.
import QtQml

QtObject {
    property int id: 0
    property QtObject target: null
    property QtObject source: null
    property int state: 6 // PwLinkState.Active
}
