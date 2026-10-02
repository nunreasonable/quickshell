#pragma once

// INetworkListManager (internet reachability) + GetAdaptersAddresses/NotifyIpInterfaceChange
// (ethernet link state) backing Network::hasInternet and Network::ethernetConnected. See
// network.hpp for the QML-facing singleton this feeds.

#include <qobject.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

struct INetworkListManager;

namespace qs::windows::sys {

class Network;

///! Everything here runs on the Qt GUI thread. `INetworkListManagerEvents::ConnectivityChanged`
/// and the `NotifyIpInterfaceChange` callback are both documented as being invoked without
/// regard to the calling thread's apartment (same as the Core Audio notifications in
/// services/pipewire/pw_backend.cpp) -- they re-post themselves onto the GUI thread via
/// `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` before touching anything here.
class NetworkConnectivityBackend: public QObject {
	Q_OBJECT;

public:
	explicit NetworkConnectivityBackend(Network* owner);
	~NetworkConnectivityBackend() override;
	Q_DISABLE_COPY_MOVE(NetworkConnectivityBackend);

	void start();

	void refreshConnectivity();
	void refreshEthernet();

	// Called by the (free-threaded) sink/callback; re-enters on the GUI thread.
	void handleConnectivityChanged();
	void handleInterfaceChanged();

private:
	Network* mOwner;
	INetworkListManager* mManager = nullptr;
	void* mConnectivitySink = nullptr; // NlmEventsSink*, owns one ref
	void* mConnectionPoint = nullptr; // IConnectionPoint*, owns one ref, held for Unadvise
	DWORD mAdviseCookie = 0;
	HANDLE mIpChangeHandle = nullptr;
};

} // namespace qs::windows::sys
