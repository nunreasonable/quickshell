#pragma once

// Small helpers shared by the Core Audio notification callback objects in this module.
//
// Every callback (IAudioEndpointVolumeCallback, IMMNotificationClient, IAudioSessionEvents,
// IAudioSessionNotification) is invoked by Core Audio on an arbitrary MTA worker thread, never
// the Qt GUI thread that owns the QObjects it reports to. Callbacks therefore only ever read
// plain data out of their arguments and hand it to the GUI thread via
// `QMetaObject::invokeMethod(target, ..., Qt::QueuedConnection)`; they never touch a QObject
// directly.
//
// The remaining hazard is the callback object outliving (or racing) the QObject it points at:
// Core Audio refcounts these callback objects itself, so one can be mid-callback on a worker
// thread at the exact moment the GUI thread destroys the target. Callers are expected to
// synchronously Unregister/Unsubscribe before tearing down the target (which Core Audio
// guarantees stops *new* callbacks), and additionally call `detach()` right before destroying
// the target, which blocks until any callback already in flight has read `target()` under the
// same mutex -- closing the narrow window between unregistering and the target actually going
// away.

#include <qmutex.h>

namespace qs::windows::services::pipewire {

template <typename T> class ComCallbackTarget {
public:
	explicit ComCallbackTarget(T* target): mTarget(target) {}
	virtual ~ComCallbackTarget() = default;

	void detach() {
		QMutexLocker locker(&this->mMutex);
		this->mTarget = nullptr;
	}

protected:
	[[nodiscard]] T* target() {
		QMutexLocker locker(&this->mMutex);
		return this->mTarget;
	}

private:
	QMutex mMutex;
	T* mTarget;
};

} // namespace qs::windows::services::pipewire
