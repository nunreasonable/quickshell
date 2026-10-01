pragma Singleton
// Windows shim for Quickshell.Bluetooth's BluetoothAdapterState.
// Real backend: WinRT Windows.Devices.Radios / Bluetooth APIs.
import QtQml

QtObject {
    enum Enum {
        Disabled = 0,
        Enabled = 1,
        Enabling = 2,
        Disabling = 3,
        Blocked = 4
    }

    function toString(state) {
        switch (state) {
        case BluetoothAdapterState.Enabled: return "Enabled";
        case BluetoothAdapterState.Enabling: return "Enabling";
        case BluetoothAdapterState.Disabling: return "Disabling";
        case BluetoothAdapterState.Blocked: return "Blocked";
        default: return "Disabled";
        }
    }
}
