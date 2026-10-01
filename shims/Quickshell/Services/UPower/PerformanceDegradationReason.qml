pragma Singleton
// Windows shim for Quickshell.Services.UPower's PerformanceDegradationReason.
// Real backend: Windows power schemes. Not used by ii today; kept for API
// completeness.
import QtQml

QtObject {
    enum Enum {
        None = 0,
        LapDetected = 1,
        HighTemperature = 2
    }

    function toString(reason) {
        switch (reason) {
        case PerformanceDegradationReason.LapDetected: return "LapDetected";
        case PerformanceDegradationReason.HighTemperature: return "HighTemperature";
        default: return "None";
        }
    }
}
