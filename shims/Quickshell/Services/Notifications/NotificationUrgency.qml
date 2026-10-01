pragma Singleton
// Windows shim for Quickshell.Services.Notifications' NotificationUrgency.
// Real backend: whatever the eventual Windows notification broker reports
// (there's no direct Windows equivalent of freedesktop urgency levels, so
// a future backend will likely infer this from toast priority/duration).
import QtQml

QtObject {
    enum Enum {
        Low = 0,
        Normal = 1,
        Critical = 2
    }

    function toString(value) {
        switch (value) {
        case NotificationUrgency.Low: return "Low";
        case NotificationUrgency.Critical: return "Critical";
        default: return "Normal";
        }
    }
}
