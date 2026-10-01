#pragma once

// The Core Audio engine backing the `Pipewire` QML singleton: device + session enumeration,
// default-endpoint tracking and the notification plumbing that keeps it all live. Everything
// public here runs on the Qt GUI thread; see com_util.hpp for how the COM callbacks (which run
// on Core Audio's own worker threads) get back onto it safely.

#include <qhash.h>
#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>

struct IMMDevice;
struct IMMDeviceEnumerator;
struct IAudioSessionManager2;
struct IAudioSessionControl;
struct IAudioSessionControl2;
struct IAudioMeterInformation;

namespace qs::windows::services::pipewire {

class Pipewire;
class PwNode;
class PwLinkGroup;

class PwBackend: public QObject {
	Q_OBJECT;

public:
	explicit PwBackend(Pipewire* owner);
	~PwBackend() override;
	Q_DISABLE_COPY_MOVE(PwBackend);

	void start();

	// Sets `node`'s endpoint as the system default (console + multimedia + communications
	// roles) via the undocumented IPolicyConfig interface. Logs and no-ops if unavailable.
	void setPreferredDefault(PwNode* node, bool isSink);

	// --- Called by the COM notification glue, always already marshalled onto the GUI thread ---
	void handleDefaultDeviceChanged(int flow, int role, const QString& deviceId);
	void handleDeviceAdded(const QString& deviceId);
	void handleDeviceRemoved(const QString& deviceId);
	void handleDeviceStateChanged(const QString& deviceId, quint32 newState);
	// Takes ownership of one reference to `control` (from IAudioSessionNotification::
	// OnSessionCreated, already AddRef'd per COM convention for callback out-params).
	void adoptNewSession(const QString& endpointId, IAudioSessionControl* control);
	void handleSessionVolumeChanged(const QString& sessionKey, float volume, bool muted);
	void handleSessionStateChanged(const QString& sessionKey, int newState);
	void handleSessionDisconnected(const QString& sessionKey);

private:
	struct EndpointEntry {
		PwNode* node = nullptr;
		IAudioSessionManager2* sessionManager = nullptr;
		void* sessionNotification = nullptr; // SessionNotificationClient*, owns one ref
		int flow = 0; // EDataFlow
	};

	struct SessionEntry {
		PwNode* node = nullptr;
		QString endpointId;
		void* eventsCallback = nullptr; // SessionEventsCallback*, owns one ref
		IAudioSessionControl2* control = nullptr; // owns one ref
		bool capturing = false; // whether a capture PwLinkGroup is currently published
	};

	void enumerateExistingDevices(int flow);
	PwNode* createEndpointNode(IMMDevice* device, int flow);
	void removeEndpoint(const QString& deviceId);
	void enumerateSessionsFor(const QString& endpointId);
	// Takes ownership of one reference to `control` (from IAudioSessionEnumerator::GetSession
	// or adoptNewSession, both already-AddRef'd per COM out-param convention).
	void addSessionNode(const QString& endpointId, IAudioSessionControl* control);
	void removeSessionNode(const QString& sessionKey);
	void refreshDefault(int flow);
	void tryCreatePolicyConfig();

	Pipewire* owner;
	IMMDeviceEnumerator* enumerator = nullptr;
	void* notificationClient = nullptr; // MMNotificationClient*, owns one ref
	void* policyConfig = nullptr; // IPolicyConfig*, may stay null if unavailable
	bool policyConfigAttempted = false;

	QHash<QString, EndpointEntry> endpoints; // key: IMMDevice id
	QHash<QString, SessionEntry> sessions; // key: synthetic "pid-instanceId" session key
	QHash<QString, PwLinkGroup*> captureLinkGroups; // key: session key

	quint32 nextId = 1;
};

} // namespace qs::windows::services::pipewire
