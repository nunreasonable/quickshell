#pragma once

// WlanAPI wrapper backing Network's Wi-Fi properties and Network::networks. See network.hpp
// for the QML-facing singleton this feeds.

#include <vector>

#include <qobject.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

#include <wlanapi.h>

namespace qs::windows::sys {

class Network;

// Plain snapshot of one visible network, passed from NetworkWifiBackend to
// Network::backendUpdateAvailableNetworks (which turns it into/updates a NetworkWifiNetwork).
struct RawWifiNetwork {
	QString ssid;
	QString bssid; // "" if no BSS match was found for it (see network_wifi.cpp)
	int strength = 0; // 0..100
	int frequency = 0; // MHz, 0 if unknown
	bool active = false;
	QString security; // "" for open
	bool hasProfile = false;
};

///! Everything here runs on the Qt GUI thread except WlanAPI's own notification callback:
/// the WLAN service invokes that on one of its own worker threads (no apartment/thread
/// affinity requirements, unlike the COM/WinRT backends elsewhere in this module -- see
/// docs/AGENTS.md), and it immediately re-posts itself onto the GUI thread via
/// `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` before touching anything here.
///
/// Only the first WLAN interface found is driven (matches a typical machine with at most one
/// Wi-Fi adapter; ii has no concept of picking between several).
class NetworkWifiBackend: public QObject {
	Q_OBJECT;

public:
	explicit NetworkWifiBackend(Network* owner);
	~NetworkWifiBackend() override;
	Q_DISABLE_COPY_MOVE(NetworkWifiBackend);

	// Opens the WLAN handle, enumerates interfaces and registers for notifications. No-ops
	// (leaving `Network::wifiAdapterPresent` false) if there's no WLAN interface at all, e.g.
	// the ethernet-only test VM this was developed against.
	void start();

	void refreshRadioState();
	// Best-effort: WlanSetInterface's software radio state can require admin; on failure this
	// falls back to opening the Windows Wi-Fi settings page and leaves the radio untouched.
	void setRadioEnabled(bool enabled);
	void scan();
	void refreshAvailableNetworks();
	void refreshCurrentConnection();
	// Connects with the existing saved profile when `hasProfile`, otherwise builds a new
	// open/WPA2-personal/WPA3-personal profile first (`password` is ignored when `secure` is
	// false). Result arrives asynchronously via Network::backendWifiConnectResult.
	void connectToNetwork(const QString& ssid, const QString& password, bool hasProfile, bool secure, bool wpa3);
	void disconnectActive();
	void forgetNetwork(const QString& ssid);

private:
	void closeHandle();
	static void WINAPI notificationCallback(PWLAN_NOTIFICATION_DATA data, PVOID context);
	void handleAcmNotification(DWORD code);
	void handleMsmNotification(DWORD code);

	Network* mOwner;
	HANDLE mHandle = nullptr;
	bool mHasAdapter = false;
	GUID mInterfaceGuid {};
};

} // namespace qs::windows::sys
