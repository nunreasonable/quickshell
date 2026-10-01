// Windows shim for Quickshell.Bluetooth's BluetoothDevice.
// Real backend: WinRT Windows.Devices.Bluetooth / DeviceInformation APIs.
//
// Upstream is QML_UNCREATABLE and only ever handed out via
// Bluetooth.devices / BluetoothAdapter.devices, both of which are always
// empty in this phase (no device enumeration wired up yet), so no instance
// of this type exists at runtime today, but it's registered for API
// completeness (ii type-annotates delegate `required property
// BluetoothDevice` in a few places).
import QtQml

QtObject {
    id: root

    property string address: ""
    property string name: ""
    readonly property string deviceName: root.name
    readonly property string icon: ""
    property int state: BluetoothDeviceState.Disconnected
    property bool connected: false
    property bool paired: false
    property bool bonded: false
    readonly property bool pairing: false
    property bool trusted: false
    property bool blocked: false
    property bool wakeAllowed: false
    readonly property bool batteryAvailable: false
    readonly property real battery: 0.0
    readonly property QtObject adapter: null

    function connect() {
        console.info("[shim] BluetoothDevice.connect (no-op)", root.name);
        root.connected = true;
        root.state = BluetoothDeviceState.Connected;
    }

    function disconnect() {
        console.info("[shim] BluetoothDevice.disconnect (no-op)", root.name);
        root.connected = false;
        root.state = BluetoothDeviceState.Disconnected;
    }

    function pair() {
        console.info("[shim] BluetoothDevice.pair (no-op)", root.name);
        root.paired = true;
    }

    function cancelPair() {
        console.info("[shim] BluetoothDevice.cancelPair (no-op)", root.name);
    }

    function forget() {
        console.info("[shim] BluetoothDevice.forget (no-op)", root.name);
        root.paired = false;
        root.trusted = false;
        root.connected = false;
    }
}
