#pragma once

// Connecting / disconnecting Bluetooth audio devices. Windows has no public "connect this
// paired device" API; what it does have is the KSPROPSETID_BtAudio property set on the kernel
// streaming filters of the Bluetooth audio drivers (A2DP sink, hands-free). A GET of
// KSPROPERTY_ONESHOT_RECONNECT / KSPROPERTY_ONESHOT_DISCONNECT on a filter asks the driver to
// (re)establish or drop that profile's connection; Microsoft documents it for exactly this use
// (apps reconnecting a paired Bluetooth audio device). The filters are reached from the device's
// Core Audio endpoints (matched by container id) through IDeviceTopology, the documented way to
// get an IKsControl for an audio device.
//
// Must run on a COM-initialized thread (the Bluetooth worker's MTA).

#include <qt_windows.h>
#include <qtypes.h>

namespace qs::bluetooth::ks {

struct OneShotResult {
	int endpoints = 0; // audio endpoints (any state) whose container id matched
	int filters = 0;   // distinct KS filters behind those endpoints
	int sent = 0;      // filters that accepted the property
	HRESULT lastError = S_OK;
};

/// Sends KSPROPERTY_ONESHOT_RECONNECT (`reconnect`) or KSPROPERTY_ONESHOT_DISCONNECT to every KS
/// filter behind the audio endpoints of the device container `containerId`.
OneShotResult sendOneShot(const GUID& containerId, bool reconnect);

/// Number of Core Audio endpoints (any state) that belong to `containerId`.
int countAudioEndpoints(const GUID& containerId);

} // namespace qs::bluetooth::ks
