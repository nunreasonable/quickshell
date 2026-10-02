#pragma once

#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../../core/doc.hpp"
#include "../../core/model.hpp"
#include "backend_types.hpp"

namespace qs::bluetooth {

///! Power state of a Bluetooth adapter.
class BluetoothAdapterState: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		/// The adapter is powered off.
		Disabled = 0,
		/// The adapter is powered on.
		Enabled = 1,
		/// The adapter is transitioning from off to on.
		Enabling = 2,
		/// The adapter is transitioning from on to off.
		Disabling = 3,
		/// The adapter is blocked: on Windows, turned off by a hardware switch or the firmware
		/// (`RadioState.Disabled`), so it can't be enabled from software.
		Blocked = 4,
	};
	Q_ENUM(Enum);

	Q_INVOKABLE static QString toString(qs::bluetooth::BluetoothAdapterState::Enum state);
};

class BluetoothDevice;

///! A Bluetooth adapter
/// Windows backend for `Quickshell.Bluetooth`'s BluetoothAdapter: the default
/// `Windows.Devices.Bluetooth.BluetoothAdapter` and its `Windows.Devices.Radios` radio.
/// Windows only ever uses one Bluetooth radio at a time, so there is at most one adapter.
///
/// > [!NOTE] Windows has no public API for adapter discoverability or the pairable window:
/// > @@discoverable stays false (Windows makes the PC discoverable while its Bluetooth settings
/// > page is open), and @@pairable, @@pairableTimeout and @@discoverableTimeout are kept but
/// > have no effect.
class BluetoothAdapter: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_UNCREATABLE("");
	// clang-format off
	/// System provided name of the adapter. See @@adapterId for the internal identifier.
	Q_PROPERTY(QString name READ default NOTIFY nameChanged BINDABLE bindableName);
	/// True if the adapter is currently enabled. More detailed state is available from @@state.
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	/// Detailed power state of the adapter.
	Q_PROPERTY(qs::bluetooth::BluetoothAdapterState::Enum state READ default NOTIFY stateChanged BINDABLE bindableState);
	/// True if the adapter can be discovered by other bluetooth devices.
	Q_PROPERTY(bool discoverable READ discoverable WRITE setDiscoverable NOTIFY discoverableChanged);
	/// Timeout in seconds for how long the adapter stays discoverable after @@discoverable is set to true.
	/// A value of 0 means the adapter stays discoverable forever.
	Q_PROPERTY(quint32 discoverableTimeout READ discoverableTimeout WRITE setDiscoverableTimeout NOTIFY discoverableTimeoutChanged);
	/// True if the adapter is scanning for new devices.
	Q_PROPERTY(bool discovering READ discovering WRITE setDiscovering NOTIFY discoveringChanged);
	/// True if the adapter is accepting incoming pairing requests.
	///
	/// This only affects incoming pairing requests and should typically only be changed
	/// by system settings applications. Defaults to true.
	Q_PROPERTY(bool pairable READ pairable WRITE setPairable NOTIFY pairableChanged);
	/// Timeout in seconds for how long the adapter stays pairable after @@pairable is set to true.
	/// A value of 0 means the adapter stays pairable forever. Defaults to 0.
	Q_PROPERTY(quint32 pairableTimeout READ pairableTimeout WRITE setPairableTimeout NOTIFY pairableTimeoutChanged);
	/// Bluetooth devices connected to this adapter.
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::bluetooth::BluetoothDevice>*);
	Q_PROPERTY(UntypedObjectModel* devices READ devices CONSTANT);
	/// The internal ID of the adapter (on Windows, its device interface id).
	Q_PROPERTY(QString adapterId READ adapterId CONSTANT);
	/// Upstream: DBus path of the adapter. On Windows, the same device interface id as @@adapterId.
	Q_PROPERTY(QString dbusPath READ path CONSTANT);
	// clang-format on

public:
	explicit BluetoothAdapter(const AdapterSnapshot& snapshot, QObject* parent = nullptr);

	[[nodiscard]] QString path() const { return this->mId; }
	[[nodiscard]] QString adapterId() const { return this->mId; }

	[[nodiscard]] bool enabled() const { return this->bEnabled; }
	void setEnabled(bool enabled);

	[[nodiscard]] bool discoverable() const { return this->bDiscoverable; }
	void setDiscoverable(bool discoverable);

	[[nodiscard]] bool discovering() const { return this->bDiscovering; }
	void setDiscovering(bool discovering);

	[[nodiscard]] quint32 discoverableTimeout() const { return this->bDiscoverableTimeout; }
	void setDiscoverableTimeout(quint32 timeout);

	[[nodiscard]] bool pairable() const { return this->bPairable; }
	void setPairable(bool pairable);

	[[nodiscard]] quint32 pairableTimeout() const { return this->bPairableTimeout; }
	void setPairableTimeout(quint32 timeout);

	[[nodiscard]] QBindable<QString> bindableName() { return &this->bName; }
	[[nodiscard]] QBindable<bool> bindableEnabled() { return &this->bEnabled; }
	[[nodiscard]] QBindable<BluetoothAdapterState::Enum> bindableState() { return &this->bState; }
	[[nodiscard]] QBindable<bool> bindableDiscoverable() { return &this->bDiscoverable; }
	[[nodiscard]] QBindable<quint32> bindableDiscoverableTimeout() {
		return &this->bDiscoverableTimeout;
	}
	[[nodiscard]] QBindable<bool> bindableDiscovering() { return &this->bDiscovering; }
	[[nodiscard]] QBindable<bool> bindablePairable() { return &this->bPairable; }
	[[nodiscard]] QBindable<quint32> bindablePairableTimeout() { return &this->bPairableTimeout; }
	[[nodiscard]] ObjectModel<BluetoothDevice>* devices() { return &this->mDevices; }

	void startDiscovery();
	void stopDiscovery();

	// From WinBluetooth (GUI thread).
	void applySnapshot(const AdapterSnapshot& snapshot);
	void applyDiscovering(bool discovering);

signals:
	void nameChanged();
	void enabledChanged();
	void stateChanged();
	void discoverableChanged();
	void discoverableTimeoutChanged();
	void discoveringChanged();
	void pairableChanged();
	void pairableTimeoutChanged();

private:
	void applyPower(RadioPower power);

	QString mId;
	ObjectModel<BluetoothDevice> mDevices {this};
	quint64 mPowerSeq = 0; // last power request sent to the worker
	bool mDiscoverableWarned = false;
	bool mUnknownPowerWarned = false;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothAdapter, QString, bName, &BluetoothAdapter::nameChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothAdapter, bool, bEnabled, &BluetoothAdapter::enabledChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothAdapter, BluetoothAdapterState::Enum, bState, &BluetoothAdapter::stateChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothAdapter, bool, bDiscoverable, &BluetoothAdapter::discoverableChanged);
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(BluetoothAdapter, quint32, bDiscoverableTimeout, 180, &BluetoothAdapter::discoverableTimeoutChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothAdapter, bool, bDiscovering, &BluetoothAdapter::discoveringChanged);
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(BluetoothAdapter, bool, bPairable, true, &BluetoothAdapter::pairableChanged);
	Q_OBJECT_BINDABLE_PROPERTY(BluetoothAdapter, quint32, bPairableTimeout, &BluetoothAdapter::pairableTimeoutChanged);
	// clang-format on
};

} // namespace qs::bluetooth

QDebug operator<<(QDebug debug, const qs::bluetooth::BluetoothAdapter* adapter);
