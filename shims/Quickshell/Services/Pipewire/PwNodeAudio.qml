// Windows shim for Quickshell.Services.Pipewire's PwNodeAudio.
// Real backend: Core Audio (IAudioEndpointVolume for hardware
// sinks/sources, ISimpleAudioVolume for per-stream volume).
//
// Unlike upstream, these properties are always valid here (the "must be
// bound via PwObjectTracker" caveat doesn't apply, since the shim has no
// unbound/bound distinction -- everything is already "live").
import QtQml

QtObject {
    property bool muted: false
    property real volume: 0.5
    property var channels: []
    property var volumes: []
}
