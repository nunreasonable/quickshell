// Windows shim for Quickshell.Bluetooth's BluetoothAdapter.
// Real backend: WinRT Windows.Devices.Radios (power state) / Windows.
// Devices.Bluetooth (adapter info, discovery).
//
// Upstream is QML_UNCREATABLE; the one instance in use here is created by
// Bluetooth.qml as the fake `defaultAdapter`. Starts powered off with no
// paired devices -- a plausible "Bluetooth present but off" default.
import QtQml
import "../../_common" as Common

QtObject {
    id: root

    property string name: "Bluetooth"
    property bool enabled: false
    property int state: root.enabled ? BluetoothAdapterState.Enabled : BluetoothAdapterState.Disabled
    property bool discoverable: false
    property int discoverableTimeout: 180
    property bool discovering: false
    property bool pairable: true
    property int pairableTimeout: 0
    readonly property Common.ObjectModel devices: Common.ObjectModel {}
    readonly property string adapterId: "shim0"

    function startDiscovery() {
        console.info("[shim] BluetoothAdapter.startDiscovery (no-op)");
        root.discovering = true;
    }

    function stopDiscovery() {
        console.info("[shim] BluetoothAdapter.stopDiscovery (no-op)");
        root.discovering = false;
    }
}
