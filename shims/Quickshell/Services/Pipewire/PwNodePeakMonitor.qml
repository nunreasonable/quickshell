// Windows shim for Quickshell.Services.Pipewire's PwNodePeakMonitor.
// Real backend: Core Audio IAudioMeterInformation. Not used by ii today;
// kept for API completeness. Always reports silence.
import QtQml

QtObject {
    property QtObject node: null
    property bool enabled: true
    property var peaks: []
    property real peak: 0.0
    property var channels: []
}
