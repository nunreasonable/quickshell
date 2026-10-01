pragma Singleton
// Windows shim for Quickshell.Services.UPower's UPowerDeviceState.
// Real backend: SYSTEM_POWER_STATUS / IOCTL_BATTERY_QUERY_INFORMATION.
import QtQml

QtObject {
    enum Enum {
        Unknown = 0,
        Charging = 1,
        Discharging = 2,
        Empty = 3,
        FullyCharged = 4,
        PendingCharge = 5,
        PendingDischarge = 6
    }

    function toString(status) {
        switch (status) {
        case UPowerDeviceState.Charging: return "Charging";
        case UPowerDeviceState.Discharging: return "Discharging";
        case UPowerDeviceState.Empty: return "Empty";
        case UPowerDeviceState.FullyCharged: return "FullyCharged";
        case UPowerDeviceState.PendingCharge: return "PendingCharge";
        case UPowerDeviceState.PendingDischarge: return "PendingDischarge";
        default: return "Unknown";
        }
    }
}
