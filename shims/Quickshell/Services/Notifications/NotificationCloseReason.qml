pragma Singleton
// Windows shim for Quickshell.Services.Notifications' NotificationCloseReason.
// Real backend: whatever the eventual Windows notification broker reports.
import QtQml

QtObject {
    enum Enum {
        Expired = 1,
        Dismissed = 2,
        CloseRequested = 3
    }

    function toString(value) {
        switch (value) {
        case NotificationCloseReason.Expired: return "Expired";
        case NotificationCloseReason.CloseRequested: return "CloseRequested";
        default: return "Dismissed";
        }
    }
}
