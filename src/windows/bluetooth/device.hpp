#pragma once

#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "backend_types.hpp"

namespace qs::bluetooth {

///! Connection state of a Bluetooth device.
class BluetoothDeviceState: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		/// The device is not connected.
		Disconnected = 0,
		/// The device is connected.
		Connected = 1,
		/// The device is disconnecting.
		Disconnecting = 2,
		/// The device is connecting.
		Connecting = 3,
	};
	Q_ENUM(Enum);

	Q_INVOKABLE static QString toString(qs::bluetooth::BluetoothDeviceState::Enum state);
};

class BluetoothAdapter;

///! A tracked Bluetooth device.
/// Windows backend for `Quickshell.Bluetooth`'s BluetoothDevice: one per remote address, built
/// from the device's association endpoints (classic and/or LE) as Windows' DeviceWatcher reports
/// them, with connection state from `BluetoothDevice`/`BluetoothLEDevice.ConnectionStatus` once
/// paired.
///
/// > [!NOTE] Windows differences:
/// > - @@connect() and @@disconnect() only work for audio devices (headsets, speakers): Windows
/// >   has no public connect API, so they send the Bluetooth audio driver's reconnect/disconnect
/// >   request. Other devices reconnect on their own when used; for them the calls log a warning
/// >   and do nothing. Calling @@connect() on an unpaired device pairs it instead (Windows then
/// >   connects it by itself).
/// > - @@pair() accepts "just works" and numeric comparison pairing without asking (like BlueZ
/// >   with no agent); devices that need a PIN typed on either side are rejected with a logged
/// >   reason and must be paired from Windows Settings.
/// > - @@trusted and @@bonded follow @@paired; @@blocked and @@wakeAllowed are always false, and
/// >   writing them, or @@name, isn't supported.
class BluetoothDevice: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_UNCREATABLE("");
	Q_MOC_INCLUDE("adapter.hpp");
	// clang-format off
	/// MAC address of the device.
	Q_PROPERTY(QString address READ default NOTIFY addressChanged BINDABLE bindableAddress);
	/// The name of the Bluetooth device. This property may be written to create an alias, or set to
	/// an empty string to fall back to the device provided name.
	///
	/// See @@deviceName for the name provided by the device.
	Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged);
	/// The name of the Bluetooth device, ignoring user provided aliases. See also @@name
	/// which returns a user provided alias if set.
	Q_PROPERTY(QString deviceName READ default NOTIFY deviceNameChanged BINDABLE bindableDeviceName);
	/// System icon representing the device type. Use @@Quickshell.Quickshell.iconPath() to display this in an image.
	Q_PROPERTY(QString icon READ default NOTIFY iconChanged BINDABLE bindableIcon);
	/// Connection state of the device.
	Q_PROPERTY(qs::bluetooth::BluetoothDeviceState::Enum state READ default NOTIFY stateChanged BINDABLE bindableState);
	/// True if the device is currently connected to the computer.
	///
	/// Setting this property is equivalent to calling @@connect() and @@disconnect().
	///
	/// > [!NOTE] @@state provides more detailed information if required.
	Q_PROPERTY(bool connected READ connected WRITE setConnected NOTIFY connectedChanged);
	/// True if the device is paired to the computer.
	///
	/// > [!NOTE] @@pair() can be used to pair a device, however you must @@forget() the device to unpair it.
	Q_PROPERTY(bool paired READ default NOTIFY pairedChanged BINDABLE bindablePaired);
	/// True if pairing information is stored for future connections.
	Q_PROPERTY(bool bonded READ default NOTIFY bondedChanged BINDABLE bindableBonded);
	/// True if the device is currently being paired.
	///
	/// > [!NOTE] @@cancelPair() can be used to cancel the pairing process.
	Q_PROPERTY(bool pairing READ pairing NOTIFY pairingChanged);
	/// True if the device is considered to be trusted by the system.
	/// Trusted devices are allowed to reconnect themselves to the system without intervention.
	Q_PROPERTY(bool trusted READ trusted WRITE setTrusted NOTIFY trustedChanged);
	/// True if the device is blocked from connecting.
	/// If a device is blocked, any connection attempts will be immediately rejected by the system.
	Q_PROPERTY(bool blocked READ blocked WRITE setBlocked NOTIFY blockedChanged);
	/// True if the device is allowed to wake up the host system from suspend.
	Q_PROPERTY(bool wakeAllowed READ wakeAllowed WRITE setWakeAllowed NOTIFY wakeAllowedChanged);
	/// True if the connected device reports its battery level. Battery level can be accessed via @@battery.
	Q_PROPERTY(bool batteryAvailable READ batteryAvailable NOTIFY batteryAvailableChanged);
	/// Battery level of the connected device, from `0.0` to `1.0`. Only valid if @@batteryAvailable is true.
	Q_PROPERTY(qreal battery READ default NOTIFY batteryChanged BINDABLE bindableBattery);
	/// The Bluetooth adapter this device belongs to.
	Q_PROPERTY(qs::bluetooth::BluetoothAdapter* adapter READ adapter NOTIFY adapterChanged);
	/// Upstream: DBus path of the device. On Windows, the device's association endpoint id.
	Q_PROPERTY(QString dbusPath READ path CONSTANT);
	// clang-format on

public:
	explicit BluetoothDevice(const DeviceSnapshot& snapshot, QObject* parent = nullptr);

	/// Attempt to connect to the device.
	Q_INVOKABLE void connect();
	/// Disconnect from the device.
	Q_INVOKABLE void disconnect();
	/// Attempt to pair the device.
	///
	/// > [!NOTE] @@paired and @@pairing return the current pairing status of the device.
	Q_INVOKABLE void pair();
	/// Cancel an active pairing attempt.
	Q_INVOKABLE void cancelPair();
	/// Forget the device.
	Q_INVOKABLE void forget();

	[[nodiscard]] QString key() const { return this->mKey; }
	[[nodiscard]] QString path() const { return this->mPath; }

	[[nodiscard]] bool batteryAvailable() const { return this->mBatteryAvailable; }
	[[nodiscard]] BluetoothAdapter* adapter() const;

	[[nodiscard]] bool connected() const { return this->bConnected; }
	void setConnected(bool connected);

	[[nodiscard]] bool trusted() const { return this->bTrusted; }
	void setTrusted(bool trusted);

	[[nodiscard]] bool blocked() const { return this->bBlocked; }
	void setBlocked(bool blocked);

	[[nodiscard]] QString name() const { return this->bName; }
	void setName(const QString& name);

	[[nodiscard]] bool wakeAllowed() const { return this->bWakeAllowed; }
	void setWakeAllowed(bool wakeAllowed);

	[[nodiscard]] bool pairing() const { return this->bPairing; }

	[[nodiscard]] QBindable<QString> bindableAddress() { return &this->bAddress; }
	[[nodiscard]] QBindable<QString> bindableDeviceName() { return &this->bDeviceName; }
	[[nodiscard]] QBindable<QString> bindableName() { return &this->bName; }
	[[nodiscard]] QBindable<bool> bindableConnected() { return &this->bConnected; }
	[[nodiscard]] QBindable<bool> bindablePaired() { return &this->bPaired; }
	[[nodiscard]] QBindable<bool> bindableBonded() { return &this->bBonded; }
	[[nodiscard]] QBindable<bool> bindableTrusted() { return &this->bTrusted; }
	[[nodiscard]] QBindable<bool> bindableBlocked() { return &this->bBlocked; }
	[[nodiscard]] QBindable<bool> bindableWakeAllowed() { return &this->bWakeAllowed; }
	[[nodiscard]] QBindable<QString> bindableIcon() { return &this->bIcon; }
	[[nodiscard]] QBindable<qreal> bindableBattery() { return &this->bBattery; }
	[[nodiscard]] QBindable<BluetoothDeviceState::Enum> bindableState() { return &this->bState; }

	// From WinBluetooth (GUI thread).
	void applySnapshot(const DeviceSnapshot& snapshot);
	void applyPairFinished(bool paired);
	void applyConnectFinished(bool connect, ConnectResult result);

signals:
	void addressChanged();
	void deviceNameChanged();
	void nameChanged();
	void connectedChanged();
	void stateChanged();
	void pairedChanged();
	void bondedChanged();
	void pairingChanged();
	void trustedChanged();
	void blockedChanged();
	void wakeAllowedChanged();
	void iconChanged();
	void batteryAvailableChanged();
	void batteryChanged();
	void adapterChanged();

private:
	void onConnectedChanged();
	void settleState();
	void warnUnsupported(const char* what);

	QString mKey;
	QString mPath;
	bool mBatteryAvailable = false;
	bool mUnsupportedWarned = false;
	// A (dis)connect request was sent; if no connection change follows, fall back to the real
	// state instead of staying in Connecting/Disconnecting forever.
	QTimer mStateTimeout;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, QString, bAddress, &BluetoothDevice::addressChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, QString, bDeviceName, &BluetoothDevice::deviceNameChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, QString, bName, &BluetoothDevice::nameChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, bool, bConnected, &BluetoothDevice::onConnectedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, bool, bPaired, &BluetoothDevice::pairedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, bool, bBonded, &BluetoothDevice::bondedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, bool, bTrusted, &BluetoothDevice::trustedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, bool, bBlocked, &BluetoothDevice::blockedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, bool, bWakeAllowed, &BluetoothDevice::wakeAllowedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, QString, bIcon, &BluetoothDevice::iconChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, qreal, bBattery, &BluetoothDevice::batteryChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, BluetoothDeviceState::Enum, bState, &BluetoothDevice::stateChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothDevice, bool, bPairing, &BluetoothDevice::pairingChanged);
	// clang-format on
};

} // namespace qs::bluetooth

QDebug operator<<(QDebug debug, const qs::bluetooth::BluetoothDevice* device);
