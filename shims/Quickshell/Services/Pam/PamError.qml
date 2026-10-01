pragma Singleton
// Windows shim for Quickshell.Services.Pam's PamError.
// Real backend: whatever Windows credential API eventually backs
// authentication here. Not used by ii today; kept for API completeness.
import QtQml

QtObject {
    enum Enum {
        StartFailed = 1,
        TryAuthFailed = 2,
        InternalError = 3
    }

    function toString(value) {
        switch (value) {
        case PamError.StartFailed: return "StartFailed";
        case PamError.TryAuthFailed: return "TryAuthFailed";
        default: return "InternalError";
        }
    }
}
