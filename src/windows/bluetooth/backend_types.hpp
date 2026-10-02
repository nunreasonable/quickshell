#pragma once

// Plain-Qt data passed between the WinRT worker thread (bt_worker.*) and the GUI-thread QML
// objects. Nothing in here may hold a WinRT or COM object: snapshots are copied across threads
// through queued invokeMethod lambdas.

#include <qstring.h>
#include <qtypes.h>

namespace qs::bluetooth {

/// Power state of the radio as Windows.Devices.Radios reports it, mapped by the GUI side to
/// BluetoothAdapterState.
enum class RadioPower : quint8 {
	Unknown, // no Radio object (access denied or not reported) or RadioState::Unknown
	Off,
	On,
	Blocked, // RadioState::Disabled: off by a hardware switch / firmware, can't be turned on
};

struct AdapterSnapshot {
	QString id;      // WinRT DeviceId of the BluetoothAdapter
	QString name;    // DeviceInformation name of the adapter (falls back to the radio's name)
	QString address; // "AA:BB:CC:DD:EE:FF"
	RadioPower power = RadioPower::Unknown;
	// Newest power request (numbered by the GUI) the worker has finished with. The GUI ignores
	// `power` while one of its own requests is still newer than this, which keeps it in
	// Enabling/Disabling instead of flipping back on an intermediate radio state.
	quint64 powerSeqDone = 0;
};

struct DeviceSnapshot {
	QString key;        // lowercase remote address, stable id of the device across transports
	QString path;       // DeviceInformation id of the main transport (classic preferred)
	QString address;    // "AA:BB:CC:DD:EE:FF"
	QString name;       // friendly name (BlueZ "Alias"): includes renames, address if nameless
	QString deviceName; // name reported by the device itself
	QString icon;       // freedesktop-style icon name derived from the class of device/appearance
	bool paired = false;
	bool connected = false;
	bool batteryAvailable = false;
	qreal battery = 0; // 0..1

	bool operator==(const DeviceSnapshot& other) const = default;
};

/// Outcome of a connect()/disconnect() request.
enum class ConnectResult : quint8 {
	Sent,        // KS one-shot sent to every audio filter; the real change arrives as an event
	Unsupported, // not an audio device: Windows has no public connect/disconnect API for it
	Failed,
};

} // namespace qs::bluetooth
