#include "pw_node.hpp"

#include <qcontainerfwd.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qt_windows.h>
#include <qtypes.h>
#include <qvector.h>

#include <audioclient.h>
#include <endpointvolume.h>

#include "com_util.hpp"

namespace qs::windows::services::pipewire {

namespace {
Q_LOGGING_CATEGORY(logPwNode, "quickshell.windows.pipewire.node", QtWarningMsg);

class EndpointVolumeCallback final
    : public IAudioEndpointVolumeCallback
    , public ComCallbackTarget<PwNodeAudio> {
public:
	explicit EndpointVolumeCallback(PwNodeAudio* target): ComCallbackTarget(target) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
		if (ppvObject == nullptr) return E_POINTER;

		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
			*ppvObject = static_cast<IAudioEndpointVolumeCallback*>(this);
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

	HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
		if (data == nullptr) return S_OK;

		auto* target = this->target();
		if (target == nullptr) return S_OK;

		auto volume = data->fMasterVolume;
		auto muted = data->bMuted != FALSE;

		QMetaObject::invokeMethod(
		    target,
		    [target, volume, muted]() { target->applyVolumeMuted(volume, muted); },
		    Qt::QueuedConnection
		);

		return S_OK;
	}

private:
	volatile LONG mRefCount = 1;
};

} // namespace

PwNodeAudio::~PwNodeAudio() {
	if (this->mEndpointVolume != nullptr) {
		auto* callback = static_cast<EndpointVolumeCallback*>(this->mEndpointCallback);
		if (callback != nullptr) {
			this->mEndpointVolume->UnregisterControlChangeNotify(callback);
			callback->detach();
			callback->Release();
		}

		this->mEndpointVolume->Release();
	}

	if (this->mSessionVolume != nullptr) {
		this->mSessionVolume->Release();
	}
}

void PwNodeAudio::bindEndpoint(IAudioEndpointVolume* endpointVolume) {
	this->mEndpointVolume = endpointVolume;

	auto* callback = new EndpointVolumeCallback(this);
	auto hr = endpointVolume->RegisterControlChangeNotify(callback);
	if (FAILED(hr)) {
		qCWarning(logPwNode) << "RegisterControlChangeNotify failed:" << Qt::hex << hr;
	}

	this->mEndpointCallback = callback;
}

void PwNodeAudio::bindSession(ISimpleAudioVolume* sessionVolume) {
	this->mSessionVolume = sessionVolume;
}

bool PwNodeAudio::isMuted() const {
	if (this->mEndpointVolume != nullptr) {
		BOOL muted = FALSE;
		this->mEndpointVolume->GetMute(&muted);
		return muted != FALSE;
	}

	if (this->mSessionVolume != nullptr) {
		BOOL muted = FALSE;
		this->mSessionVolume->GetMute(&muted);
		return muted != FALSE;
	}

	return false;
}

void PwNodeAudio::setMuted(bool muted) {
	HRESULT hr = S_OK;
	if (this->mEndpointVolume != nullptr) {
		hr = this->mEndpointVolume->SetMute(muted ? TRUE : FALSE, nullptr);
	} else if (this->mSessionVolume != nullptr) {
		hr = this->mSessionVolume->SetMute(muted ? TRUE : FALSE, nullptr);
	} else {
		return;
	}

	if (FAILED(hr)) {
		qCWarning(logPwNode) << "SetMute failed:" << Qt::hex << hr;
		return;
	}

	emit this->mutedChanged();
}

float PwNodeAudio::volume() const {
	if (this->mEndpointVolume != nullptr) {
		float level = 0.0F;
		this->mEndpointVolume->GetMasterVolumeLevelScalar(&level);
		return level;
	}

	if (this->mSessionVolume != nullptr) {
		float level = 0.0F;
		this->mSessionVolume->GetMasterVolume(&level);
		return level;
	}

	return 0.0F;
}

void PwNodeAudio::setVolume(float volume) {
	if (volume < 0.0F) volume = 0.0F;
	if (volume > 1.0F) volume = 1.0F;

	HRESULT hr = S_OK;
	if (this->mEndpointVolume != nullptr) {
		hr = this->mEndpointVolume->SetMasterVolumeLevelScalar(volume, nullptr);
	} else if (this->mSessionVolume != nullptr) {
		hr = this->mSessionVolume->SetMasterVolume(volume, nullptr);
	} else {
		return;
	}

	if (FAILED(hr)) {
		qCWarning(logPwNode) << "SetMasterVolume failed:" << Qt::hex << hr;
		return;
	}

	emit this->volumeChanged();
}

QVector<PwAudioChannel::Enum> PwNodeAudio::channels() const {
	if (this->mEndpointVolume == nullptr) return {};

	UINT count = 0;
	if (FAILED(this->mEndpointVolume->GetChannelCount(&count))) return {};

	return QVector<PwAudioChannel::Enum>(static_cast<qsizetype>(count), PwAudioChannel::Unknown);
}

QVector<float> PwNodeAudio::volumes() const {
	if (this->mEndpointVolume == nullptr) return {};

	UINT count = 0;
	if (FAILED(this->mEndpointVolume->GetChannelCount(&count))) return {};

	QVector<float> volumes;
	volumes.reserve(static_cast<qsizetype>(count));

	for (UINT i = 0; i < count; i++) {
		float level = 0.0F;
		this->mEndpointVolume->GetChannelVolumeLevelScalar(i, &level);
		volumes.append(level);
	}

	return volumes;
}

void PwNodeAudio::setVolumes(const QVector<float>& volumes) {
	if (this->mEndpointVolume == nullptr) return;

	for (qsizetype i = 0; i < volumes.length(); i++) {
		auto level = volumes.at(i);
		if (level < 0.0F) level = 0.0F;
		if (level > 1.0F) level = 1.0F;
		this->mEndpointVolume->SetChannelVolumeLevelScalar(static_cast<UINT>(i), level, nullptr);
	}

	emit this->volumesChanged();
	emit this->volumeChanged();
}

void PwNodeAudio::applyVolumeMuted(float volume, bool muted) {
	Q_UNUSED(volume);
	Q_UNUSED(muted);
	emit this->volumeChanged();
	emit this->mutedChanged();
	emit this->volumesChanged();
}

PwNode::PwNode(QObject* parent): QObject(parent) {
	this->mAudio = new PwNodeAudio(this);
}

PwNode::~PwNode() {
	if (this->mMeter != nullptr) this->mMeter->Release();
}

void PwNode::setProperties(const QVariantMap& properties) {
	if (this->mProperties == properties) return;
	this->mProperties = properties;
	emit this->propertiesChanged();
}

void PwNode::setReady(bool ready) {
	if (this->mReady == ready) return;
	this->mReady = ready;
	emit this->readyChanged();
}

void PwNode::setMeterInformation(IAudioMeterInformation* meter) { this->mMeter = meter; }

} // namespace qs::windows::services::pipewire
