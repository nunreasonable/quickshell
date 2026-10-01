pragma Singleton
// Windows shim for Quickshell.Services.SystemTray's Category.
// Upstream declares this as a Q_NAMESPACE (not a QML_SINGLETON), but the
// access pattern (`Category.Hardware`) is identical either way. Not read by
// ii today; kept for API completeness.
import QtQml

QtObject {
    enum Enum {
        Hardware = 0,
        SystemServices = 1,
        ApplicationStatus = 2,
        Communications = 3
    }
}
