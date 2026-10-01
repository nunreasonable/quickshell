pragma Singleton
// Windows shim for the Quickshell.Bluetooth "Bluetooth" singleton.
// Real backend: WinRT Windows.Devices.Radios/Bluetooth APIs.
//
// Exposes exactly one fake adapter (powered off, no paired devices) as
// `defaultAdapter` so ii's null-safe `Bluetooth.defaultAdapter?.foo` reads
// always have a real object to bind to instead of null.
import QtQml
import "../../_common" as Common

QtObject {
    id: root

    property QtObject defaultAdapter: BluetoothAdapter {}

    readonly property Common.ObjectModel adapters: Common.ObjectModel { values: [root.defaultAdapter] }
    readonly property Common.ObjectModel devices: Common.ObjectModel {}
}
