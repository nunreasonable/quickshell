#pragma once

#include <qstring.h>
#include <qtypes.h>

namespace qs::bluetooth {

// Icon names in the freedesktop/BlueZ style ("audio-headset", "input-mouse", "phone", ...) that
// ii's Icons.getBluetoothDeviceMaterialSymbol / WIcons.bluetoothDeviceIcon match on. Written from
// the Bluetooth SIG Assigned Numbers (class of device and GAP appearance tables). Empty when the
// class doesn't say anything useful, which ii shows as a generic Bluetooth icon.

/// `cod` is the 24-bit class of device (BluetoothClassOfDevice::RawValue layout).
QString iconForClassOfDevice(quint32 cod);

/// `appearance` is the 16-bit GAP appearance (category in bits 15-6, subcategory in bits 5-0).
QString iconForAppearance(quint16 appearance);

} // namespace qs::bluetooth
