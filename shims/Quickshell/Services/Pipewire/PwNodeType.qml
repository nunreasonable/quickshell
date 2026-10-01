pragma Singleton
// Windows shim for Quickshell.Services.Pipewire's PwNodeType flags enum.
// Real backend: Core Audio (CoreAudio/EndpointManager-equivalent on Windows
// would be WASAPI device "data flow" + "role" metadata).
import QtQml

QtObject {
    enum Flag {
        Untracked = 0,
        Audio = 1,
        Video = 2,
        Stream = 4,
        Source = 8,
        Sink = 16,
        AudioSink = 17, // Audio | Sink
        AudioSource = 9, // Audio | Source
        AudioDuplex = 25, // Audio | Sink | Source
        AudioOutStream = 21, // Audio | Sink | Stream
        AudioInStream = 13, // Audio | Source | Stream
        VideoSource = 10, // Video | Source
        VideoSink = 18 // Video | Sink
    }

    function toString(type) {
        switch (type) {
        case PwNodeType.AudioSink: return "AudioSink";
        case PwNodeType.AudioSource: return "AudioSource";
        case PwNodeType.AudioDuplex: return "AudioDuplex";
        case PwNodeType.AudioOutStream: return "AudioOutStream";
        case PwNodeType.AudioInStream: return "AudioInStream";
        case PwNodeType.VideoSource: return "VideoSource";
        case PwNodeType.VideoSink: return "VideoSink";
        case PwNodeType.Audio: return "Audio";
        case PwNodeType.Video: return "Video";
        case PwNodeType.Stream: return "Stream";
        case PwNodeType.Source: return "Source";
        case PwNodeType.Sink: return "Sink";
        default: return "Untracked";
        }
    }
}
