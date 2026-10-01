pragma Singleton
// Windows shim for Quickshell.Services.SystemTray's Status.
// Upstream declares this as a Q_NAMESPACE (not a QML_SINGLETON), but the
// access pattern ii uses (`Status.Passive`) is identical either way.
import QtQml

QtObject {
    enum Enum {
        Passive = 0,
        Active = 1,
        NeedsAttention = 2
    }
}
