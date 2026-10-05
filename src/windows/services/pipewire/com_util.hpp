#pragma once

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
