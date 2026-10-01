// Windows shim for Quickshell.Services.Mpris's MprisPlayer.
// Real backend: GSMTC (Windows.Media.Control.GlobalSystemMediaTransport
// ControlsSessionManager), one instance per session.
//
// Upstream is QML_UNCREATABLE and only ever handed out by Mpris.players,
// which is always empty in this phase (no GSMTC backend wired up yet), so no
// instance of this type exists at runtime today. It's still registered so
// the module's API surface is complete and any future native backend can
// hand out real instances without ii-facing changes. The control functions
// below update the player's own state optimistically (as a real backend
// mostly would) and log what was requested.
import QtQml

QtObject {
    id: root

    property bool canControl: false
    property bool canPlay: false
    property bool canPause: false
    property bool canTogglePlaying: false
    property bool canSeek: false
    property bool canGoNext: false
    property bool canGoPrevious: false
    property bool canQuit: false
    property bool canRaise: false
    property bool canSetFullscreen: false

    property string identity: ""
    property string desktopEntry: ""
    readonly property string dbusName: ""

    property real position: 0
    readonly property bool positionSupported: false
    readonly property real length: 0
    readonly property bool lengthSupported: false

    property real volume: 1.0
    readonly property bool volumeSupported: false

    property var metadata: ({})
    property int uniqueId: 0
    property string trackTitle: ""
    property string trackArtist: ""
    readonly property string trackArtists: root.trackArtist
    property string trackAlbum: ""
    property string trackAlbumArtist: ""
    property string trackArtUrl: ""

    property int playbackState: MprisPlaybackState.Stopped
    property bool isPlaying: false
    property int loopState: MprisLoopState.None
    readonly property bool loopSupported: false

    property real rate: 1.0
    readonly property real minRate: 1.0
    readonly property real maxRate: 1.0

    property bool shuffle: false
    readonly property bool shuffleSupported: false

    property bool fullscreen: false

    readonly property var supportedUriSchemes: []
    readonly property var supportedMimeTypes: []

    signal trackChanged()
    signal postTrackChanged()

    function raise() {
        console.info("[shim] MprisPlayer.raise", root.identity);
    }

    function quit() {
        console.info("[shim] MprisPlayer.quit", root.identity);
    }

    function openUri(uri) {
        console.info("[shim] MprisPlayer.openUri", uri);
    }

    function next() {
        console.info("[shim] MprisPlayer.next", root.identity);
    }

    function previous() {
        console.info("[shim] MprisPlayer.previous", root.identity);
    }

    function seek(offset) {
        root.position += offset;
    }

    function play() {
        root.isPlaying = true;
        root.playbackState = MprisPlaybackState.Playing;
    }

    function pause() {
        root.isPlaying = false;
        root.playbackState = MprisPlaybackState.Paused;
    }

    function stop() {
        root.isPlaying = false;
        root.playbackState = MprisPlaybackState.Stopped;
    }

    function togglePlaying() {
        if (root.isPlaying) root.pause();
        else root.play();
    }
}
