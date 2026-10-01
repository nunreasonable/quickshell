pragma Singleton
// Windows shim for Quickshell.Services.Pipewire's PwLinkState enum.
// Real backend: Core Audio. Not used by ii today; kept for API completeness.
import QtQml

QtObject {
    enum Enum {
        Error = 0,
        Unlinked = 1,
        Init = 2,
        Negotiating = 3,
        Allocating = 4,
        Paused = 5,
        Active = 6
    }

    function toString(state) {
        switch (state) {
        case PwLinkState.Unlinked: return "Unlinked";
        case PwLinkState.Init: return "Init";
        case PwLinkState.Negotiating: return "Negotiating";
        case PwLinkState.Allocating: return "Allocating";
        case PwLinkState.Paused: return "Paused";
        case PwLinkState.Active: return "Active";
        default: return "Error";
        }
    }
}
