#include "loopback.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <qt_windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qscopeguard.h>
#include <qstring.h>
#include <qtypes.h>

#include "clock.hpp"

namespace qs::windows::recorder {

namespace {
Q_LOGGING_CATEGORY(logLoopback, "quickshell.windows.recorder", QtWarningMsg);

// Shared mode buffer; capture polls far more often than this, it only needs to cover a stall.
constexpr REFERENCE_TIME BUFFER_DURATION = 10'000'000;
constexpr DWORD POLL_MS = 10;

constexpr float HALF_POWER = 0.70710678F; // -3 dB, the usual weight for folding a channel in

std::array<float, 2> speakerWeights(DWORD speaker) {
	switch (speaker) {
	case SPEAKER_FRONT_LEFT: return {1.0F, 0.0F};
	case SPEAKER_FRONT_RIGHT: return {0.0F, 1.0F};
	case SPEAKER_FRONT_CENTER:
	case SPEAKER_BACK_CENTER:
	case SPEAKER_TOP_CENTER:
	case SPEAKER_TOP_FRONT_CENTER:
	case SPEAKER_TOP_BACK_CENTER: return {HALF_POWER, HALF_POWER};
	case SPEAKER_BACK_LEFT:
	case SPEAKER_SIDE_LEFT:
	case SPEAKER_FRONT_LEFT_OF_CENTER:
	case SPEAKER_TOP_FRONT_LEFT:
	case SPEAKER_TOP_BACK_LEFT: return {HALF_POWER, 0.0F};
	case SPEAKER_BACK_RIGHT:
	case SPEAKER_SIDE_RIGHT:
	case SPEAKER_FRONT_RIGHT_OF_CENTER:
	case SPEAKER_TOP_FRONT_RIGHT:
	case SPEAKER_TOP_BACK_RIGHT: return {0.0F, HALF_POWER};
	default: return {0.0F, 0.0F}; // LFE and anything unknown
	}
}

qint16 toInt16(float sample) {
	return static_cast<qint16>(std::lround(std::clamp(sample, -1.0F, 1.0F) * 32767.0F));
}

} // namespace

LoopbackCapture::~LoopbackCapture() {
	this->stop();
	if (this->stopEvent != nullptr) CloseHandle(this->stopEvent);
}

bool LoopbackCapture::open(QString* error) {
	auto fail = [error](HRESULT hr, const QString& what) {
		*error = QString("%1 (0x%2)").arg(what).arg(static_cast<quint32>(hr), 8, 16, QChar('0'));
		qCWarning(logLoopback) << "Loopback capture:" << *error;
		return false;
	};

	winrt::com_ptr<IMMDeviceEnumerator> enumerator;
	auto hr = CoCreateInstance(
	    __uuidof(MMDeviceEnumerator),
	    nullptr,
	    CLSCTX_ALL,
	    IID_PPV_ARGS(enumerator.put())
	);
	if (FAILED(hr)) return fail(hr, "no audio device enumerator");

	winrt::com_ptr<IMMDevice> device;
	hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
	if (FAILED(hr)) return fail(hr, "no audio output device");

	hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, this->client.put_void());
	if (FAILED(hr)) return fail(hr, "cannot open the audio output device");

	WAVEFORMATEX* mix = nullptr;
	hr = this->client->GetMixFormat(&mix);
	if (FAILED(hr)) return fail(hr, "no mix format");
	auto freeMix = qScopeGuard([mix] { CoTaskMemFree(mix); });

	auto tag = mix->wFormatTag;
	DWORD mask = 0;
	auto extensible = mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	if (tag == WAVE_FORMAT_EXTENSIBLE && extensible) {
		const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix); // NOLINT
		// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT are the format tag inside the base audio GUID;
		// comparing that part avoids pulling the GUID definitions in.
		if (ext->SubFormat.Data2 == 0x0000 && ext->SubFormat.Data3 == 0x0010) {
			tag = static_cast<WORD>(ext->SubFormat.Data1);
		}
		mask = ext->dwChannelMask;
	}

	auto bits = mix->wBitsPerSample;
	if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) this->type = SampleType::Float32;
	else if (tag == WAVE_FORMAT_PCM && bits == 16) this->type = SampleType::Int16;
	else if (tag == WAVE_FORMAT_PCM && bits == 24) this->type = SampleType::Int24;
	else if (tag == WAVE_FORMAT_PCM && bits == 32) this->type = SampleType::Int32;
	else {
		return fail(E_NOTIMPL, QString("unsupported mix format (tag %1, %2 bits)").arg(tag).arg(bits));
	}

	this->inChannels = mix->nChannels;
	this->inRate = static_cast<int>(mix->nSamplesPerSec);
	this->bytesPerSample = mix->wBitsPerSample / 8;
	if (this->inChannels < 1 || this->inRate <= 0
	    || mix->nBlockAlign != this->inChannels * this->bytesPerSample)
	{
		return fail(E_NOTIMPL, "unsupported mix format layout");
	}

	this->weights.assign(this->inChannels, {0.0F, 0.0F});
	if (this->inChannels == 1) {
		this->weights[0] = {1.0F, 1.0F};
	} else if (mask == 0 || this->inChannels == 2) {
		this->weights[0] = {1.0F, 0.0F};
		this->weights[1] = {0.0F, 1.0F};
	} else {
		// Channels come in the order of the mask's bits.
		auto channel = 0;
		for (DWORD bit = 1; bit != 0 && channel < this->inChannels; bit <<= 1) {
			if ((mask & bit) == 0) continue;
			this->weights[channel++] = speakerWeights(bit);
		}

		// Keep a full scale signal on every channel from clipping after the fold.
		std::array<float, 2> sums {};
		for (const auto& w: this->weights) {
			sums[0] += w[0];
			sums[1] += w[1];
		}
		for (auto& w: this->weights) {
			if (sums[0] > 1.0F) w[0] /= sums[0];
			if (sums[1] > 1.0F) w[1] /= sums[1];
		}
	}

	this->outRate = this->inRate == 44100 || this->inRate == 48000 ? this->inRate : 48000;
	this->resampleStep = static_cast<double>(this->inRate) / this->outRate;

	hr = this->client->Initialize(
	    AUDCLNT_SHAREMODE_SHARED,
	    AUDCLNT_STREAMFLAGS_LOOPBACK,
	    BUFFER_DURATION,
	    0,
	    mix,
	    nullptr
	);
	if (FAILED(hr)) return fail(hr, "cannot start loopback capture");

	hr = this->client->GetService(IID_PPV_ARGS(this->capture.put()));
	if (FAILED(hr)) return fail(hr, "no capture service");

	this->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (this->stopEvent == nullptr) {
		return fail(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent failed");
	}

	qCInfo(logLoopback) << "Loopback:" << this->inChannels << "channels at" << this->inRate
	                    << "Hz, type" << static_cast<int>(this->type) << "->" << this->outRate
	                    << "Hz stereo";
	return true;
}

void LoopbackCapture::start(qint64 origin) {
	if (this->thread.joinable() || !this->client) return;
	this->origin = origin;
	this->thread = std::thread([this] { this->run(); });
}

void LoopbackCapture::stop() {
	if (!this->thread.joinable()) return;
	SetEvent(this->stopEvent);
	this->thread.join();
}

std::vector<qint16> LoopbackCapture::take() {
	std::vector<qint16> result;
	std::lock_guard lock(this->mutex);
	std::swap(result, this->pending);
	return result;
}

qint64 LoopbackCapture::framesAt(qint64 time) const {
	return (time - this->origin) * this->outRate / 10'000'000;
}

void LoopbackCapture::appendSilenceLocked(qint64 frames) {
	this->pending.insert(this->pending.end(), static_cast<size_t>(frames) * CHANNELS, 0);
	this->written += frames;
}

void LoopbackCapture::run() {
	// The audio client lives in the MTA; this thread joins it to keep using it.
	auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	auto hr = this->client->Start();
	if (FAILED(hr)) {
		qCWarning(logLoopback) << "IAudioClient::Start failed:" << Qt::hex << hr;
		this->lost = true;
	}

	while (WaitForSingleObject(this->stopEvent, POLL_MS) == WAIT_TIMEOUT) {
		if (!this->lost) this->drain();

		// Nothing plays, so nothing arrives: pad with silence up to a margin behind now that is
		// well past the engine's latency, so a packet that is merely late isn't overwritten.
		auto target = this->framesAt(qpc100ns()) - this->outRate / 10;
		std::lock_guard lock(this->mutex);
		if (target > this->written) this->appendSilenceLocked(target - this->written);
	}

	this->client->Stop();
	if (SUCCEEDED(com)) CoUninitialize();
}

void LoopbackCapture::drain() {
	UINT32 packet = 0;
	HRESULT hr = S_OK;

	while (SUCCEEDED(hr = this->capture->GetNextPacketSize(&packet)) && packet > 0) {
		BYTE* data = nullptr;
		UINT32 frames = 0;
		DWORD flags = 0;
		UINT64 qpcPosition = 0;

		hr = this->capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPosition);
		if (FAILED(hr)) break;
		if (hr == AUDCLNT_S_BUFFER_EMPTY) break;

		this->consume(data, frames, flags, qpcPosition);
		this->capture->ReleaseBuffer(frames);
	}

	if (FAILED(hr)) {
		// Usually AUDCLNT_E_DEVICE_INVALIDATED: the default output changed or was unplugged. The
		// rest of the recording gets silence rather than failing as a whole.
		qCWarning(logLoopback) << "Loopback capture stopped:" << Qt::hex << hr;
		this->lost = true;
	}
}

void LoopbackCapture::consume(const BYTE* data, UINT32 frames, DWORD flags, UINT64 qpcPosition) {
	if (frames == 0) return;

	// Downmix to float stereo.
	this->scratch.assign(static_cast<size_t>(frames) * CHANNELS, 0.0F);

	if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data != nullptr) {
		auto blockAlign = this->inChannels * this->bytesPerSample;

		for (UINT32 f = 0; f < frames; ++f) {
			const auto* frame = data + static_cast<size_t>(f) * blockAlign;
			auto left = 0.0F;
			auto right = 0.0F;

			for (auto c = 0; c < this->inChannels; ++c) {
				const auto* p = frame + static_cast<ptrdiff_t>(c) * this->bytesPerSample;
				float sample = 0;

				switch (this->type) {
				case SampleType::Float32: std::memcpy(&sample, p, sizeof(float)); break;
				case SampleType::Int16: {
					qint16 v = 0;
					std::memcpy(&v, p, sizeof(v));
					sample = static_cast<float>(v) / 32768.0F;
				} break;
				case SampleType::Int24: {
					auto v = static_cast<qint32>(
					    static_cast<quint32>(p[0]) << 8 | static_cast<quint32>(p[1]) << 16
					    | static_cast<quint32>(p[2]) << 24
					);
					sample = static_cast<float>(v >> 8) / 8388608.0F;
				} break;
				case SampleType::Int32: {
					qint32 v = 0;
					std::memcpy(&v, p, sizeof(v));
					sample = static_cast<float>(v) / 2147483648.0F;
				} break;
				}

				left += sample * this->weights[c][0];
				right += sample * this->weights[c][1];
			}

			this->scratch[f * 2] = left;
			this->scratch[f * 2 + 1] = right;
		}
	}

	std::vector<qint16> out;

	if (this->inRate == this->outRate) {
		out.resize(this->scratch.size());
		std::ranges::transform(this->scratch, out.begin(), toInt16);
	} else {
		// Linear interpolation; resamplePos is the next output sample's position in input
		// frames relative to this packet, -1 meaning the previous packet's last frame.
		out.reserve(static_cast<size_t>(frames / this->resampleStep + 2) * CHANNELS);
		auto pos = this->resamplePos;

		while (pos < static_cast<double>(frames) - 1) {
			auto index = static_cast<qint64>(std::floor(pos));
			auto frac = static_cast<float>(pos - static_cast<double>(index));

			for (auto c = 0; c < CHANNELS; ++c) {
				auto a = index < 0 ? this->resamplePrev[c] : this->scratch[index * CHANNELS + c];
				auto b = this->scratch[(index + 1) * CHANNELS + c];
				out.push_back(toInt16(a + (b - a) * frac));
			}

			pos += this->resampleStep;
		}

		this->resamplePos = pos - frames;
		this->resamplePrev = {this->scratch[(frames - 1) * 2], this->scratch[(frames - 1) * 2 + 1]};
	}

	auto count = static_cast<qint64>(out.size() / CHANNELS);
	if (count == 0) return;

	std::lock_guard lock(this->mutex);

	// Line the packet up with the video clock. Small differences are jitter; a packet starting
	// later than expected follows a pause in playback (filled with silence), one starting
	// earlier overlaps silence already padded in or predates the recording (trimmed).
	auto start = this->written;
	if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0) {
		start = this->framesAt(static_cast<qint64>(qpcPosition));
	}

	auto slack = static_cast<qint64>(this->outRate / 50); // 20 ms
	qint64 skip = 0;

	if (start > this->written + slack) this->appendSilenceLocked(start - this->written);
	else if (start + slack < this->written) skip = std::min(this->written - start, count);

	this->pending.insert(this->pending.end(), out.begin() + skip * CHANNELS, out.end());
	this->written += count - skip;
}

} // namespace qs::windows::recorder
