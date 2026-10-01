// Windows shim for Quickshell.Services.UPower's UPowerDevice.
// Real backend: GetSystemPowerStatus / battery class driver IOCTLs.
//
// Upstream is QML_UNCREATABLE and only ever handed out by UPower.qml
// (displayDevice, or an entry of devices); instances here are likewise only
// created by UPower.qml. Defaults describe "no battery present", matching a
// desktop machine -- UPower.qml's displayDevice uses exactly these defaults.
import QtQml

QtObject {
    property int type: UPowerDeviceType.Unknown
    property bool powerSupply: false
    property real energy: 0.0
    property real energyCapacity: 0.0
    property real changeRate: 0.0
    property real timeToEmpty: 0
    property real timeToFull: 0
    property real percentage: 0.0
    property bool isPresent: false
    property int state: UPowerDeviceState.Unknown
    property real healthPercentage: 100.0
    property bool healthSupported: false
    property string iconName: ""
    property bool isLaptopBattery: false
    property string nativePath: ""
    property string model: ""
    property bool ready: true
}
