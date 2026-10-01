pragma Singleton
// Windows shim for Quickshell.Services.UPower's PowerProfile.
// Real backend: Windows power schemes (PowerSetActiveScheme et al).
import QtQml

QtObject {
    enum Enum {
        PowerSaver = 0,
        Balanced = 1,
        Performance = 2
    }

    function toString(profile) {
        switch (profile) {
        case PowerProfile.PowerSaver: return "PowerSaver";
        case PowerProfile.Performance: return "Performance";
        default: return "Balanced";
        }
    }
}
