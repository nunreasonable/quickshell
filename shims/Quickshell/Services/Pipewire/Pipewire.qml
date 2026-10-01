pragma Singleton
// Windows shim for the Quickshell.Services.Pipewire "Pipewire" singleton.
// Real backend: Core Audio (WASAPI device enumeration + session volume).
//
// Exposes exactly one fake sink ("Speakers") and one fake source
// ("Microphone"), both always ready with a writable PwNodeAudio so volume
// sliders and mute toggles have something real to bind to. `links` and
// `linkGroups` are always empty (no routing graph is modeled yet), so
// anything derived from them (e.g. ii's mic/camera "in use" detection) will
// always report nothing in use.
import QtQml
import "../../../_common" as Common

QtObject {
    id: root

    property Component _nodeComponent: Component { PwNode {} }
    property Component _audioComponent: Component { PwNodeAudio {} }

    property QtObject _speakers: null
    property QtObject _microphone: null

    readonly property Common.ObjectModel nodes: Common.ObjectModel { id: nodesModel }
    readonly property Common.ObjectModel links: Common.ObjectModel { id: linksModel }
    readonly property Common.ObjectModel linkGroups: Common.ObjectModel { id: linkGroupsModel }

    property QtObject preferredDefaultAudioSink: root._speakers
    property QtObject preferredDefaultAudioSource: root._microphone
    readonly property QtObject defaultAudioSink: root.preferredDefaultAudioSink
    readonly property QtObject defaultAudioSource: root.preferredDefaultAudioSource

    readonly property bool ready: true

    function _init() {
        var speakersAudio = root._audioComponent.createObject(root, {
            volume: 0.5,
            muted: false,
        });
        root._speakers = root._nodeComponent.createObject(root, {
            id: 1,
            name: "shim-speakers",
            description: "Speakers",
            nickname: "Speakers",
            isSink: true,
            isStream: false,
            type: PwNodeType.AudioSink,
            properties: {"device.description": "Speakers", "media.class": "Audio/Sink"},
            audio: speakersAudio,
            ready: true,
        });

        var micAudio = root._audioComponent.createObject(root, {
            volume: 0.5,
            muted: false,
        });
        root._microphone = root._nodeComponent.createObject(root, {
            id: 2,
            name: "shim-microphone",
            description: "Microphone",
            nickname: "Microphone",
            isSink: false,
            isStream: false,
            type: PwNodeType.AudioSource,
            properties: {"device.description": "Microphone", "media.class": "Audio/Source"},
            audio: micAudio,
            ready: true,
        });

        nodesModel.values = [root._speakers, root._microphone];
    }

    Component.onCompleted: root._init()
}
