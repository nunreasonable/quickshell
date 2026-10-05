#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <qt_windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtypes.h>
#include <winrt/base.h>

namespace qs::windows::recorder {

class LoopbackCapture {
public:
	LoopbackCapture() = default;
	~LoopbackCapture();
	Q_DISABLE_COPY_MOVE(LoopbackCapture);

	bool open(QString* error);
	[[nodiscard]] int sampleRate() const { return this->outRate; }
	static constexpr int CHANNELS = 2;

	void start(qint64 origin);
	void stop();

	std::vector<qint16> take();

	[[nodiscard]] bool lost() const { return this->mLost.load(std::memory_order_relaxed); }

private:
	enum class SampleType : quint8 { Float32, Int16, Int24, Int32 };

	void run();
	void drain();
	void consume(const BYTE* data, UINT32 frames, DWORD flags, UINT64 qpcPosition);
	void appendSilenceLocked(qint64 frames);
	[[nodiscard]] qint64 framesAt(qint64 time) const;

	winrt::com_ptr<IAudioClient> client;
	winrt::com_ptr<IAudioCaptureClient> capture;
	std::thread thread;
	HANDLE stopEvent = nullptr;
	qint64 origin = 0;

	SampleType type = SampleType::Float32;
	int inChannels = 0;
	int inRate = 0;
	int bytesPerSample = 0;
	std::vector<std::array<float, 2>> weights;

	int outRate = 0;
	double resampleStep = 1.0;
	double resamplePos = 0.0;
	std::array<float, 2> resamplePrev {};
	std::vector<float> scratch;

	std::atomic<bool> mLost {false};

	std::mutex mutex;
	std::vector<qint16> pending;
	qint64 written = 0;
};

} // namespace qs::windows::recorder
