#pragma once

#include <memory>

#include <qhash.h>
#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

#include "../../core/doc.hpp"
#include "../../core/model.hpp"
#include "adapter.hpp"
#include "backend_types.hpp"
#include "device.hpp"

namespace qs::bluetooth {

class BtBackend;

///! Process-wide Bluetooth state behind the `Bluetooth` QML singleton.
/// Like upstream's Bluez object it outlives QML engine reloads (devices keep their identity),
/// and is the GUI-thread side of the WinRT backend: the worker posts snapshots here
/// (backend* calls, always queued onto this thread) and the QML objects send their commands
/// through backend().
class WinBluetooth: public QObject {
	Q_OBJECT;

public:
	static WinBluetooth* instance();

	[[nodiscard]] ObjectModel<BluetoothAdapter>* adapters() { return &this->mAdapters; }
	[[nodiscard]] ObjectModel<BluetoothDevice>* devices() { return &this->mDevices; }
	[[nodiscard]] BluetoothAdapter* adapter() const { return this->mAdapter; }
	[[nodiscard]] BtBackend* backend() const { return this->mBackend.get(); }
	/// True once the first adapter lookup finished (with or without an adapter).
	[[nodiscard]] bool isScanned() const { return this->mScanned; }

	// Called by the worker through queued invokeMethod (GUI thread only).
	void backendScanned(bool hasAdapter);
	void backendAdapterChanged(const AdapterSnapshot& snapshot);
	void backendAdapterRemoved();
	void backendDiscoveringChanged(bool discovering);
	void backendDeviceChanged(const DeviceSnapshot& snapshot);
	void backendDeviceRemoved(const QString& key);
	void backendPairFinished(const QString& key, bool paired);
	void backendConnectFinished(const QString& key, bool connect, ConnectResult result);

signals:
	void defaultAdapterChanged();
	void scanned(bool hasAdapter);

private:
	explicit WinBluetooth();
	~WinBluetooth() override;

	ObjectModel<BluetoothAdapter> mAdapters {this};
	ObjectModel<BluetoothDevice> mDevices {this};
	QHash<QString, BluetoothDevice*> mDeviceMap;
	BluetoothAdapter* mAdapter = nullptr;
	bool mScanned = false;
	std::unique_ptr<BtBackend> mBackend;

public:
	Q_OBJECT_BINDABLE_PROPERTY(
	    WinBluetooth,
	    BluetoothAdapter*,
	    bDefaultAdapter,
	    &WinBluetooth::defaultAdapterChanged
	);
};

///! Bluetooth manager
/// Provides access to bluetooth devices and adapters.
///
/// Windows backend (WinRT `Windows.Devices.Bluetooth` / `Windows.Devices.Radios` /
/// `DeviceWatcher`), same API as the BlueZ one. Without a Bluetooth adapter @@adapters and
/// @@devices are empty and @@defaultAdapter is null, as on Linux without one.
class BluetoothQml: public QObject {
	Q_OBJECT;
	QML_NAMED_ELEMENT(Bluetooth);
	QML_SINGLETON;
	// clang-format off
	/// The default bluetooth adapter. Usually there is only one.
	Q_PROPERTY(qs::bluetooth::BluetoothAdapter* defaultAdapter READ default NOTIFY defaultAdapterChanged BINDABLE bindableDefaultAdapter);
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::bluetooth::BluetoothAdapter>*);
	/// A list of all bluetooth adapters. See @@defaultAdapter for the default.
	Q_PROPERTY(UntypedObjectModel* adapters READ adapters CONSTANT);
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::bluetooth::BluetoothDevice>*);
	/// A list of all connected bluetooth devices across all adapters.
	/// See @@BluetoothAdapter.devices for the devices connected to a single adapter.
	Q_PROPERTY(UntypedObjectModel* devices READ devices CONSTANT);
	// clang-format on

signals:
	void defaultAdapterChanged();

public:
	explicit BluetoothQml();

	[[nodiscard]] static ObjectModel<BluetoothAdapter>* adapters() {
		return WinBluetooth::instance()->adapters();
	}

	[[nodiscard]] static ObjectModel<BluetoothDevice>* devices() {
		return WinBluetooth::instance()->devices();
	}

	[[nodiscard]] static QBindable<BluetoothAdapter*> bindableDefaultAdapter() {
		return &WinBluetooth::instance()->bDefaultAdapter;
	}
};

} // namespace qs::bluetooth
