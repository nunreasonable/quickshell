pragma Singleton
// Windows shim for the Quickshell.Services.UPower "UPower" singleton.
// Real backend: GetSystemPowerStatus for the aggregate display device,
// battery class driver enumeration for `devices`.
//
// `displayDevice` describes a desktop with no battery (isPresent/
// isLaptopBattery both false), which is the common case this phase targets;
// `devices` is always empty, and `onBattery` is always false.
import QtQml
import "../../../_common" as Common

QtObject {
    id: root

    property QtObject displayDevice: UPowerDevice {
        isPresent: false
        isLaptopBattery: false
        powerSupply: false
        ready: true
    }

    readonly property Common.ObjectModel devices: Common.ObjectModel {}

    readonly property bool onBattery: false
}
