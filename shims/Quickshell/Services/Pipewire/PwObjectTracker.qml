// Windows shim for Quickshell.Services.Pipewire's PwObjectTracker.
// Real backend: Core Audio. Upstream uses this to "bind" nodes so their
// properties/audio become valid; in the shim every node is already live
// regardless of binding, so this is just an inert holder for the `objects`
// list ii assigns (e.g. `PwObjectTracker { objects: [sink, source] }`).
import QtQml

QtObject {
    property var objects: []
}
