// Windows shim for Quickshell.Services.Pipewire's PwNodeLinkTracker.
// Real backend: Core Audio. Not used by ii today; kept for API completeness.
import QtQml

QtObject {
    property QtObject node: null
    property var linkGroups: []
}
