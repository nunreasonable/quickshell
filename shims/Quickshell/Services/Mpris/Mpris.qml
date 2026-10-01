pragma Singleton
// Windows shim for the Quickshell.Services.Mpris "Mpris" singleton.
// Real backend: GSMTC (GlobalSystemMediaTransportControlsSessionManager).
//
// `players` is always empty (no media sessions tracked yet). ii reads it
// three different ways: via `.values` (the documented path), as a model
// directly (`Instantiator { model: Mpris.players }`), and with bracket
// indexing / `.indexOf()` called directly on it. The shared ObjectModel
// helper covers `.values`/`.indexOf()` like every other collection in these
// shims; a bare QObject given to Instantiator's `model` (rather than a real
// list/QAbstractItemModel) is treated by Qt as a single-item model, so
// `Instantiator { model: Mpris.players }` ends up creating one delegate
// whose `modelData` fails the `MprisPlayer` type check and comes out null --
// verified against ii's actual usage (services/MprisController.qml) to be a
// harmless no-op there, since its `modelData` handlers already guard on
// `root.trackedPlayer == null` first. Bracket indexing (`Mpris.players[0]`)
// on a plain object simply yields `undefined`, which is the same "no
// players" fallback ii's code already handles with `??`.
import QtQml
import "../../../_common" as Common

QtObject {
    readonly property Common.ObjectModel players: Common.ObjectModel {}
}
