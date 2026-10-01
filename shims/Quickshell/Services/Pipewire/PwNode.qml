// Windows shim for Quickshell.Services.Pipewire's PwNode.
// Real backend: Core Audio IMMDevice (hardware nodes) / audio session (app
// stream nodes).
//
// Upstream is QML_UNCREATABLE and only ever handed out by Pipewire.qml;
// instances here are likewise only created by Pipewire.qml.
import QtQml

QtObject {
    property int id: 0
    property string name: ""
    property string description: ""
    property string nickname: ""
    property bool isSink: false
    property bool isStream: false
    property int type: 0 // PwNodeType.Untracked
    property var properties: ({})
    property QtObject audio: null
    property bool ready: true
}
