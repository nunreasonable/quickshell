// Windows shim helper (not a Quickshell.* module itself; used internally by the
// other shims via a relative directory import). Minimal stand-in for the
// subset of quickshell/src/core/model.hpp's ObjectModel<T> that ii relies on:
// a `values` list (with the automatic `valuesChanged` signal QML generates for
// list properties, so `Connections { target: someModel }` keeps working) plus
// `indexOf`. Real backend: whichever native collection owns the data on
// Windows (Hyprland/Pipewire/UPower/Bluetooth/SystemTray/Notifications each
// get their own instance of this).
import QtQml

QtObject {
    /// The contents of the model, as a plain QML list. Mirrors upstream's
    /// `values` property (READ values NOTIFY valuesChanged).
    property list<QtObject> values: []

    /// Upstream also emits these around individual insertions/removals; ii
    /// never connects to them, but they're kept for API completeness.
    signal objectInsertedPre(QtObject object, int index)
    signal objectInsertedPost(QtObject object, int index)
    signal objectRemovedPre(QtObject object, int index)
    signal objectRemovedPost(QtObject object, int index)

    /// Mirrors upstream's `Q_INVOKABLE qsizetype indexOf(QObject*)`.
    function indexOf(object) {
        return values.indexOf(object);
    }
}
