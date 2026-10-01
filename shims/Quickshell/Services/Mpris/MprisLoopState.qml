pragma Singleton
// Windows shim for Quickshell.Services.Mpris's MprisLoopState.
// Real backend: GSMTC (GlobalSystemMediaTransportControls).
import QtQml

QtObject {
    enum Enum {
        None = 0,
        Track = 1,
        Playlist = 2
    }

    function toString(status) {
        switch (status) {
        case MprisLoopState.Track: return "Track";
        case MprisLoopState.Playlist: return "Playlist";
        default: return "None";
        }
    }
}
