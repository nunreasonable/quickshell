pragma Singleton
// Windows shim for Quickshell.Services.Mpris's MprisPlaybackState.
// Real backend: GSMTC (GlobalSystemMediaTransportControls).
import QtQml

QtObject {
    enum Enum {
        Stopped = 0,
        Playing = 1,
        Paused = 2
    }

    function toString(status) {
        switch (status) {
        case MprisPlaybackState.Playing: return "Playing";
        case MprisPlaybackState.Paused: return "Paused";
        default: return "Stopped";
        }
    }
}
