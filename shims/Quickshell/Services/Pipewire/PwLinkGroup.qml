// Windows shim for Quickshell.Services.Pipewire's PwLinkGroup.
// Real backend: Core Audio. ii reads `.source`/`.target` (e.g. Privacy.qml
// to work out which apps are using the mic/camera); Pipewire.linkGroups is
// always empty in this shim, so nothing is ever reported as "in use" yet.
import QtQml

QtObject {
    property QtObject target: null
    property QtObject source: null
    property int state: 6 // PwLinkState.Active
}
