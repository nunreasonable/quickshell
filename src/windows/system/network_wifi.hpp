#pragma once

#include <vector>

#include <qobject.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

#include <wlanapi.h>

namespace qs::windows::sys {

class Network;

struct RawWifiNetwork {
	QString ssid;
	QString bssid;
	int strength = 0;
	int frequency = 0;
	bool active = false;
	QString security;
	bool hasProfile = false;
	QString profileName;
};

class NetworkWifiBackend: public QObject {
	Q_OBJECT;

public:
	explicit NetworkWifiBackend(Network* owner);
	~NetworkWifiBackend() override;
	Q_DISABLE_COPY_MOVE(NetworkWifiBackend);

	void start();

	void refreshRadioState();
	void setRadioEnabled(bool enabled);
	void scan();
	void refreshAvailableNetworks();
	void refreshCurrentConnection();
	void connectToNetwork(
	    const QString& ssid,
	    const QString& profileName,
	    const QString& password,
	    bool hasProfile,
	    bool secure,
	    bool wpa3
	);
	void disconnectActive();
	void forgetNetwork(const QString& ssid, const QString& profileName);

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
