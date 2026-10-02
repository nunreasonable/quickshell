#pragma once

#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows::sys {

///! One Wi-Fi network visible to the WLAN API.
/// Built from a `WLAN_AVAILABLE_NETWORK` entry (plus a best-effort BSS match for @@bssid and
/// the frequency band), one per visible SSID+security combination -- same granularity nmcli
/// reports. ii's platform-generic `services/network/WifiAccessPoint.qml` wraps whatever object
/// its `lastIpcObject` points at (nmcli's parsed JS object on Linux, one of these on Windows)
/// and binds these exact property names off it, so this type's shape has to match that one
/// field for field.
class NetworkWifiNetwork: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(QString ssid READ default NOTIFY ssidChanged BINDABLE bindableSsid);
	Q_PROPERTY(QString bssid READ default NOTIFY bssidChanged BINDABLE bindableBssid);
	/// Signal quality, 0-100 (WLAN_SIGNAL_QUALITY) -- same scale as nmcli's SIGNAL column.
	Q_PROPERTY(int strength READ default NOTIFY strengthChanged BINDABLE bindableStrength);
	/// Channel center frequency in MHz, or 0 if a BSS match wasn't found.
	Q_PROPERTY(int frequency READ default NOTIFY frequencyChanged BINDABLE bindableFrequency);
	Q_PROPERTY(bool active READ default NOTIFY activeChanged BINDABLE bindableActive);
	/// "" for an open network, otherwise a short label such as "WPA2" or "WPA3".
	Q_PROPERTY(QString security READ default NOTIFY securityChanged BINDABLE bindableSecurity);
	/// Whether a saved Windows profile already exists for this SSID
	/// (`WLAN_AVAILABLE_NETWORK_HAS_PROFILE`): if true, @@Network::connectToNetwork reconnects
	/// with it directly instead of asking for a password.
	Q_PROPERTY(bool hasProfile READ default NOTIFY hasProfileChanged BINDABLE bindableHasProfile);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("NetworkWifiNetworks can only be acquired from Network.networks");

public:
	explicit NetworkWifiNetwork(QObject* parent = nullptr): QObject(parent) {}
	Q_DISABLE_COPY_MOVE(NetworkWifiNetwork);

	[[nodiscard]] QBindable<QString> bindableSsid() const { return &this->bSsid; }
	[[nodiscard]] QBindable<QString> bindableBssid() const { return &this->bBssid; }
	[[nodiscard]] QBindable<int> bindableStrength() const { return &this->bStrength; }
	[[nodiscard]] QBindable<int> bindableFrequency() const { return &this->bFrequency; }
	[[nodiscard]] QBindable<bool> bindableActive() const { return &this->bActive; }
	[[nodiscard]] QBindable<QString> bindableSecurity() const { return &this->bSecurity; }
	[[nodiscard]] QBindable<bool> bindableHasProfile() const { return &this->bHasProfile; }

	// Mutated only by Network/NetworkWifiBackend (GUI thread) while rebuilding the list.
	void setSsid(const QString& v) { this->bSsid = v; }
	void setBssid(const QString& v) { this->bBssid = v; }
	void setStrength(int v) { this->bStrength = v; }
	void setFrequency(int v) { this->bFrequency = v; }
	void setActive(bool v) { this->bActive = v; }
	void setSecurity(const QString& v) { this->bSecurity = v; }
	void setHasProfile(bool v) { this->bHasProfile = v; }

signals:
	void ssidChanged();
	void bssidChanged();
	void strengthChanged();
	void frequencyChanged();
	void activeChanged();
	void securityChanged();
	void hasProfileChanged();

private:
	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(NetworkWifiNetwork, QString, bSsid, &NetworkWifiNetwork::ssidChanged);
	Q_OBJECT_BINDABLE_PROPERTY(NetworkWifiNetwork, QString, bBssid, &NetworkWifiNetwork::bssidChanged);
	Q_OBJECT_BINDABLE_PROPERTY(NetworkWifiNetwork, int, bStrength, &NetworkWifiNetwork::strengthChanged);
	Q_OBJECT_BINDABLE_PROPERTY(NetworkWifiNetwork, int, bFrequency, &NetworkWifiNetwork::frequencyChanged);
	Q_OBJECT_BINDABLE_PROPERTY(NetworkWifiNetwork, bool, bActive, &NetworkWifiNetwork::activeChanged);
	Q_OBJECT_BINDABLE_PROPERTY(NetworkWifiNetwork, QString, bSecurity, &NetworkWifiNetwork::securityChanged);
	Q_OBJECT_BINDABLE_PROPERTY(NetworkWifiNetwork, bool, bHasProfile, &NetworkWifiNetwork::hasProfileChanged);
	// clang-format on
};

} // namespace qs::windows::sys
