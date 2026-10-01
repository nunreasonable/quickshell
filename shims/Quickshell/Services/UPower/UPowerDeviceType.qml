pragma Singleton
// Windows shim for Quickshell.Services.UPower's UPowerDeviceType.
// Real backend: SYSTEM_POWER_STATUS / device enumeration. Not referenced by
// ii today beyond the type name; kept for API completeness.
import QtQml

QtObject {
    enum Enum {
        Unknown = 0,
        LinePower = 1,
        Battery = 2,
        Ups = 3,
        Monitor = 4,
        Mouse = 5,
        Keyboard = 6,
        Pda = 7,
        Phone = 8,
        MediaPlayer = 9,
        Tablet = 10,
        Computer = 11,
        GamingInput = 12,
        Pen = 13,
        Touchpad = 14,
        Modem = 15,
        Network = 16,
        Headset = 17,
        Speakers = 18,
        Headphones = 19,
        Video = 20,
        OtherAudio = 21,
        RemoteControl = 22,
        Printer = 23,
        Scanner = 24,
        Camera = 25,
        Wearable = 26,
        Toy = 27,
        BluetoothGeneric = 28
    }

    function toString(type) {
        return type === UPowerDeviceType.Battery ? "Battery" : "Unknown";
    }
}
