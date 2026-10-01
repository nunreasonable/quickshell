pragma Singleton
// Windows shim for Quickshell.Services.Pipewire's PwAudioChannel enum.
// Real backend: Core Audio channel masks. Not used by ii today; kept for
// API completeness (PwNodeAudio.channels/.volumes are also unused by ii).
import QtQml

QtObject {
    enum Enum {
        Unknown = 0,
        NA = 1,
        Mono = 2,
        FrontCenter = 3,
        FrontLeft = 4,
        FrontRight = 5,
        FrontLeftCenter = 6,
        FrontRightCenter = 7,
        FrontLeftWide = 8,
        FrontRightWide = 9,
        FrontCenterHigh = 10,
        FrontLeftHigh = 11,
        FrontRightHigh = 12,
        LowFrequencyEffects = 13,
        LowFrequencyEffects2 = 14,
        LowFrequencyEffectsLeft = 15,
        LowFrequencyEffectsRight = 16,
        SideLeft = 17,
        SideRight = 18,
        RearCenter = 19,
        RearLeft = 20,
        RearRight = 21,
        RearLeftCenter = 22,
        RearRightCenter = 23,
        TopCenter = 24,
        TopFrontCenter = 25,
        TopFrontLeft = 26,
        TopFrontRight = 27,
        TopFrontLeftCenter = 28,
        TopFrontRightCenter = 29,
        TopSideLeft = 30,
        TopSideRight = 31,
        TopRearCenter = 32,
        TopRearLeft = 33,
        TopRearRight = 34,
        BottomCenter = 35,
        BottomLeftCenter = 36,
        BottomRightCenter = 37,
        AuxRangeStart = 38,
        AuxRangeEnd = 39,
        CustomRangeStart = 40
    }

    function toString(value) {
        return "channel" + value;
    }
}
