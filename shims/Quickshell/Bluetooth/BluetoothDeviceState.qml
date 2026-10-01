pragma Singleton
// Windows shim for Quickshell.Bluetooth's BluetoothDeviceState.
// Real backend: WinRT Windows.Devices.Bluetooth APIs. Not used by ii today;
// kept for API completeness.
import QtQml

QtObject {
    enum Enum {
        Disconnected = 0,
        Connected = 1,
        Disconnecting = 2,
        Connecting = 3
    }

    function toString(state) {
        switch (state) {
        case BluetoothDeviceState.Connected: return "Connected";
        case BluetoothDeviceState.Disconnecting: return "Disconnecting";
        case BluetoothDeviceState.Connecting: return "Connecting";
        default: return "Disconnected";
        }
    }
}
