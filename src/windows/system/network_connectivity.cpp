#include "network_connectivity.hpp"

#include <cstdlib>
#include <vector>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qmutex.h>

// Must come before iphlpapi.h: IPTypes.h's GetAdaptersAddresses flags/types are guarded by
// `#ifdef _WINSOCK2API_` (winsock2.h's own include guard), and netioapi.h's interface-change
// notification API (NotifyIpInterfaceChange, MIB_IPINTERFACE_ROW, ...) needs ws2ipdef.h's
// `_WS2IPDEF_` guard defined too -- iphlpapi.h's own comments say the caller is responsible for
// both. WIN32_LEAN_AND_MEAN (set project-wide) keeps qt_windows.h's <windows.h> from pulling in
// the old winsock.h first, so this is safe.
#include <winsock2.h>
#include <ws2ipdef.h>

#include <iphlpapi.h>
#include <netlistmgr.h>
#include <objbase.h>
#include <ocidl.h>

#include "../../core/logcat.hpp"
#include "network.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logConnectivity, "quickshell.windows.network.connectivity", QtWarningMsg);

bool ensureComInitialized() {
	auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	// S_FALSE: already initialized on this thread. RPC_E_CHANGED_MODE: initialized with a
	// different concurrency model already (Qt's platform plugin does this for us) -- both fine,
	// we just piggyback on the existing apartment, same as services/pipewire/pw_backend.cpp.
	if (hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) return true;
	qCWarning(logConnectivity) << "CoInitializeEx failed:" << Qt::hex << hr;
	return false;
}

// `INetworkListManagerEvents::ConnectivityChanged` is documented as being invoked without
// regard to the calling thread's apartment, so (like the Core Audio callbacks in
// services/pipewire/com_util.hpp) this may run on an arbitrary thread. `target` is guarded by a
// mutex and cleared (via detach()) before the backend it points to goes away.
class NlmEventsSink final: public INetworkListManagerEvents {
public:
	explicit NlmEventsSink(NetworkConnectivityBackend* target): mTarget(target) {}

	void detach() {
		QMutexLocker locker(&this->mMutex);
		this->mTarget = nullptr;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
		if (ppvObject == nullptr) return E_POINTER;

		if (riid == __uuidof(IUnknown) || riid == __uuidof(INetworkListManagerEvents)) {
			*ppvObject = static_cast<INetworkListManagerEvents*>(this);
			this->AddRef();
			return S_OK;
		}

		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override {
		return static_cast<ULONG>(InterlockedIncrement(&this->mRefCount));
	}

	ULONG STDMETHODCALLTYPE Release() override {
		auto rc = InterlockedDecrement(&this->mRefCount);
		if (rc == 0) delete this;
		return static_cast<ULONG>(rc);
	}

	HRESULT STDMETHODCALLTYPE ConnectivityChanged(NLM_CONNECTIVITY /*newConnectivity*/) override {
		QMutexLocker locker(&this->mMutex);
		if (this->mTarget != nullptr) {
			auto* target = this->mTarget;
			QMetaObject::invokeMethod(
			    target,
			    [target]() { target->handleConnectivityChanged(); },
			    Qt::QueuedConnection
			);
		}
		return S_OK;
	}

private:
	volatile LONG mRefCount = 1;
	QMutex mMutex;
	NetworkConnectivityBackend* mTarget;
};

void WINAPI ipInterfaceChangeCallback(PVOID callerContext, PMIB_IPINTERFACE_ROW /*row*/, MIB_NOTIFICATION_TYPE /*type*/) {
	if (callerContext == nullptr) return;
	auto* self = static_cast<NetworkConnectivityBackend*>(callerContext);
	QMetaObject::invokeMethod(
	    self,
	    [self]() { self->handleInterfaceChanged(); },
	    Qt::QueuedConnection
	);
}

} // namespace

NetworkConnectivityBackend::NetworkConnectivityBackend(Network* owner): mOwner(owner) {}

NetworkConnectivityBackend::~NetworkConnectivityBackend() {
	if (this->mIpChangeHandle != nullptr) {
		CancelMibChangeNotify2(this->mIpChangeHandle);
		this->mIpChangeHandle = nullptr;
	}

	if (this->mConnectionPoint != nullptr) {
		auto* cp = static_cast<IConnectionPoint*>(this->mConnectionPoint);
		cp->Unadvise(this->mAdviseCookie);
		cp->Release();
		this->mConnectionPoint = nullptr;
	}

	if (this->mConnectivitySink != nullptr) {
		static_cast<NlmEventsSink*>(this->mConnectivitySink)->detach();
		static_cast<NlmEventsSink*>(this->mConnectivitySink)->Release();
		this->mConnectivitySink = nullptr;
	}

	if (this->mManager != nullptr) {
		this->mManager->Release();
		this->mManager = nullptr;
	}
}

void NetworkConnectivityBackend::start() {
	if (!ensureComInitialized()) return;

	auto hr = CoCreateInstance(
	    CLSID_NetworkListManager,
	    nullptr,
	    CLSCTX_ALL,
	    IID_INetworkListManager,
	    reinterpret_cast<LPVOID*>(&this->mManager) // NOLINT
	);
	if (FAILED(hr) || this->mManager == nullptr) {
		qCWarning(logConnectivity) << "CoCreateInstance(NetworkListManager) failed:" << Qt::hex << hr;
	} else {
		auto* sink = new NlmEventsSink(this);
		this->mConnectivitySink = sink;

		IConnectionPointContainer* cpc = nullptr;
		if (SUCCEEDED(this->mManager->QueryInterface(IID_PPV_ARGS(&cpc))) && cpc != nullptr) {
			IConnectionPoint* cp = nullptr;
			if (SUCCEEDED(cpc->FindConnectionPoint(IID_INetworkListManagerEvents, &cp)) && cp != nullptr) {
				if (SUCCEEDED(cp->Advise(sink, &this->mAdviseCookie))) {
					this->mConnectionPoint = cp; // keep the ref for Unadvise at teardown
				} else {
					cp->Release();
				}
			}
			cpc->Release();
		}

		this->refreshConnectivity();
	}

	auto err = NotifyIpInterfaceChange(
	    AF_UNSPEC,
	    &ipInterfaceChangeCallback,
	    this,
	    FALSE,
	    &this->mIpChangeHandle
	);
	if (err != NO_ERROR) {
		qCWarning(logConnectivity) << "NotifyIpInterfaceChange failed:" << err;
		this->mIpChangeHandle = nullptr;
	}

	this->refreshEthernet();
}

void NetworkConnectivityBackend::refreshConnectivity() {
	if (this->mManager == nullptr) return;

	NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
	auto hr = this->mManager->GetConnectivity(&connectivity);
	if (FAILED(hr)) {
		qCDebug(logConnectivity) << "GetConnectivity failed:" << Qt::hex << hr;
		return;
	}

	auto hasInternet =
	    (connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
	this->mOwner->backendSetHasInternet(hasInternet);
}

void NetworkConnectivityBackend::refreshEthernet() {
	ULONG size = 15000;
	std::vector<unsigned char> buffer(size);
	auto* addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); // NOLINT

	constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST
	                     | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;

	auto result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size);
	if (result == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(size);
		addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); // NOLINT
		result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size);
	}

	auto ethernetUp = false;
	if (result == ERROR_SUCCESS) {
		for (auto* curr = addresses; curr != nullptr; curr = curr->Next) {
			if (curr->IfType == IF_TYPE_ETHERNET_CSMACD && curr->OperStatus == IfOperStatusUp) {
				ethernetUp = true;
				break;
			}
		}
	} else {
		qCDebug(logConnectivity) << "GetAdaptersAddresses failed:" << result;
	}

	this->mOwner->backendSetEthernetConnected(ethernetUp);
}

void NetworkConnectivityBackend::handleConnectivityChanged() { this->refreshConnectivity(); }

void NetworkConnectivityBackend::handleInterfaceChanged() {
	this->refreshEthernet();
	this->refreshConnectivity();
}

} // namespace qs::windows::sys
