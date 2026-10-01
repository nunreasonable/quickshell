pragma Singleton
// Windows shim for the Quickshell.Services.UPower "PowerProfiles" singleton.
// Real backend: Windows power schemes (PowerSetActiveScheme/
// PowerGetActiveScheme, with the "High performance" scheme standing in for
// Performance).
//
// `hasPerformanceProfile` is true since Windows always offers a high
// performance plan on desktops, so ii's profile-cycling toggles have all
// three profiles to cycle through.
import QtQml

QtObject {
    property int profile: PowerProfile.Balanced
    readonly property bool hasPerformanceProfile: true
    readonly property int degradationReason: PerformanceDegradationReason.None
    readonly property var holds: []
}
