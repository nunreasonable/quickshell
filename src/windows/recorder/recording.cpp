#include "recording.hpp"

#include <memory>
#include <utility>

#include <qt_windows.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mfidl.h>

#include <qcoreapplication.h>
#include <qdeadlinetimer.h>
#include <qdir.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qstring.h>
#include <qthread.h>
#include <winrt/base.h>

#include "../util.hpp"
#include "clock.hpp"
#include "loopback.hpp"
#include "mp4_writer.hpp"
#include "region_capture.hpp"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace qs::windows::recorder {

namespace {
Q_LOGGING_CATEGORY(logRecorder, "quickshell.windows.recorder", QtWarningMsg);

// How long to wait for every monitor's first frame before starting the clock anyway, so the
// video doesn't open on a black frame.
constexpr qint64 PRIME_TIMEOUT = 10'000'000; // 1 s, in 100 ns units
// Ticks missed (the thread didn't get scheduled, the encoder blocked) are made up with repeats
// of the current frame to keep the frame rate constant, up to this many; beyond that the time
// is skipped.
constexpr qint64 MAX_CATCH_UP = 10;
// Finalizing writes the MP4 index; on exit the GUI thread waits this long for it.
constexpr int SHUTDOWN_WAIT_MS = 15000;

QString hrMessage(const QString& what, HRESULT hr) {
	return QString("%1 (0x%2)").arg(what).arg(static_cast<quint32>(hr), 8, 16, QChar('0'));
}

bool createDevice(winrt::com_ptr<ID3D11Device>& device, QString* error) {
	// Video support is what the sink writer's hardware encoder path needs; some drivers (or
	// the basic render driver) don't offer it, and the software path does without.
	const UINT attempts[] = {
	    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
	    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	};

	HRESULT hr = E_FAIL;
	for (auto flags: attempts) {
		device = nullptr;
		hr = D3D11CreateDevice(
		    nullptr,
		    D3D_DRIVER_TYPE_HARDWARE,
		    nullptr,
		    flags,
		    nullptr,
		    0,
		    D3D11_SDK_VERSION,
		    device.put(),
		    nullptr,
		    nullptr
		);
		if (SUCCEEDED(hr)) break;
	}

	if (FAILED(hr)) {
		*error = hrMessage("cannot create a Direct3D device", hr);
		return false;
	}

	// Capture callbacks (thread pool) and Media Foundation's own threads share the context.
	winrt::com_ptr<ID3D11DeviceContext> context;
	device->GetImmediateContext(context.put());
	if (auto multithread = context.try_as<ID3D11Multithread>()) {
		multithread->SetMultithreadProtected(TRUE);
	}

	return true;
}

QString checkAvailability() {
	if (windowsBuild() < 19041) return "screen recording needs Windows 10 version 2004 or newer";

	// Media Foundation is delay loaded: Windows N editions without the Media Feature Pack don't
	// have it, and the shell must still start there.
	for (const auto* dll: {L"mfplat.dll", L"mfreadwrite.dll"}) {
		if (LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) == nullptr) {
			return "Media Foundation is missing (on Windows N editions, install the Media Feature Pack)";
		}
	}

	auto hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(hr)) return hrMessage("Media Foundation failed to start", hr);

	MFT_REGISTER_TYPE_INFO output {MFMediaType_Video, MFVideoFormat_H264};
	IMFActivate** activates = nullptr;
	UINT32 count = 0;

	hr = MFTEnumEx(
	    MFT_CATEGORY_VIDEO_ENCODER,
	    MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE
	        | MFT_ENUM_FLAG_SORTANDFILTER,
	    nullptr,
	    &output,
	    &activates,
	    &count
	);

	if (activates != nullptr) {
		for (UINT32 i = 0; i < count; ++i) activates[i]->Release(); // NOLINT
		CoTaskMemFree(static_cast<void*>(activates));
	}

	MFShutdown();

	if (FAILED(hr) || count == 0) return "no H.264 encoder is installed";
	return {};
}

} // namespace

// --- job (recorder thread) ---------------------------------------------------------------------

class RecordingJob {
public:
	explicit RecordingJob(RecordingRequest request)
	    : request(std::move(request))
	    , stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

	~RecordingJob() {
		if (this->stopEvent != nullptr) CloseHandle(this->stopEvent);
	}

	Q_DISABLE_COPY_MOVE(RecordingJob);

	void run();
	void requestStop() const { SetEvent(this->stopEvent); }

	const RecordingRequest request;

private:
	bool record(QString* error);
	[[nodiscard]] qint64 tickTime(qint64 tick) const {
		return tick * 10'000'000 / this->request.fps;
	}

	template <typename F>
	void post(F&& function) {
		auto* controller = RecorderController::instance();
		QMetaObject::invokeMethod(controller, std::forward<F>(function), Qt::QueuedConnection);
	}

	HANDLE stopEvent;
};

void RecordingJob::run() {
	QString error;
	auto ok = false;

	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);

		auto hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
		if (SUCCEEDED(hr)) {
			ok = this->record(&error);
			MFShutdown();
		} else {
			error = hrMessage("Media Foundation failed to start", hr);
		}

		winrt::uninit_apartment();
	} catch (const winrt::hresult_error& e) {
		error = hrMessage("COM initialization failed", e.code().value);
	}

	if (!ok) {
		qCWarning(logRecorder) << "Recording failed:" << error;
		// Whatever got written isn't playable without the index.
		DeleteFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(this->request.path).utf16()));
	}

	auto* self = this;
	this->post([self, ok, error] { RecorderController::instance()->onJobDone(self, ok, error); });
}

bool RecordingJob::record(QString* error) {
	const auto& request = this->request;

	winrt::com_ptr<ID3D11Device> device;
	if (!createDevice(device, error)) return false;

	RegionCapture capture;
	if (!capture.start(device.get(), request.region, request.cursor, error)) return false;

	std::unique_ptr<LoopbackCapture> audio;
	if (request.sound) {
		QString audioError;
		audio = std::make_unique<LoopbackCapture>();

		if (!audio->open(&audioError)) {
			audio.reset();
			auto* self = this;
			this->post([self, audioError] {
				RecorderController::instance()->onJobSoundUnavailable(self, audioError);
			});
		}
	}

	Mp4Writer writer;
	auto size = request.region.size();
	auto audioRate = audio ? audio->sampleRate() : 0;
	auto opened = writer.open(request.path, size, request.fps, audioRate, device.get(), error);

	if (!opened && audio) {
		// No AAC encoder or one that wants something else: video only beats nothing.
		auto reason = *error;
		audio.reset();
		opened = writer.open(request.path, size, request.fps, 0, device.get(), error);

		if (opened) {
			auto* self = this;
			this->post([self, reason] {
				RecorderController::instance()->onJobSoundUnavailable(self, reason);
			});
		}
	}

	if (!opened) return false;

	auto stopping = false;

	auto primeDeadline = qpc100ns() + PRIME_TIMEOUT;
	while (!capture.primed() && qpc100ns() < primeDeadline) {
		if (WaitForSingleObject(this->stopEvent, 10) == WAIT_OBJECT_0) {
			stopping = true;
			break;
		}
	}

	auto origin = qpc100ns();
	if (audio) audio->start(origin);

	{
		auto* self = this;
		this->post([self] { RecorderController::instance()->onJobStarted(self); });
	}

	// The default timer resolution (15.6 ms) would make every other frame late at 30 fps.
	auto* timer = CreateWaitableTimerExW(
	    nullptr,
	    nullptr,
	    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
	    TIMER_ALL_ACCESS
	);
	if (timer == nullptr) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

	auto failed = false;
	qint64 tick = 0;

	while (true) {
		auto now = qpc100ns() - origin;

		if (now - this->tickTime(tick) > this->tickTime(MAX_CATCH_UP)) {
			auto skipTo = now * request.fps / 10'000'000;
			qCDebug(logRecorder) << "Skipping" << skipTo - tick << "frames";
			tick = skipTo;
		}

		// One frame per tick whether or not the screen changed: constant frame rate.
		while (!failed && this->tickTime(tick) <= now) {
			auto time = this->tickTime(tick);
			auto duration = this->tickTime(tick + 1) - time;
			failed = !writer.writeVideo(capture.composite(), time, duration, error);
			tick++;
		}

		if (audio && !failed) {
			auto samples = audio->take();
			auto frames = static_cast<qsizetype>(samples.size() / LoopbackCapture::CHANNELS);
			if (frames > 0) failed = !writer.writeAudio(samples.data(), frames, error);
		}

		if (failed || stopping) break;

		if (capture.lost()) {
			qCWarning(logRecorder) << "A recorded screen went away; stopping the recording.";
			break;
		}

		auto wait = this->tickTime(tick) - (qpc100ns() - origin);
		if (wait <= 0) {
			stopping = WaitForSingleObject(this->stopEvent, 0) == WAIT_OBJECT_0;
		} else if (timer != nullptr) {
			LARGE_INTEGER due {};
			due.QuadPart = -wait; // relative, 100 ns units
			SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
			HANDLE handles[] = {this->stopEvent, timer};
			stopping = WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0;
		} else {
			auto ms = static_cast<DWORD>((wait + 9'999) / 10'000);
			stopping = WaitForSingleObject(this->stopEvent, ms) == WAIT_OBJECT_0;
		}
	}

	if (timer != nullptr) CloseHandle(timer);

	if (audio) {
		audio->stop();

		if (!failed) {
			auto samples = audio->take();
			auto frames = static_cast<qsizetype>(samples.size() / LoopbackCapture::CHANNELS);
			if (frames > 0) failed = !writer.writeAudio(samples.data(), frames, error);
		}
	}

	capture.stop();

	if (failed) {
		writer.close();
		return false;
	}

	qCInfo(logRecorder) << "Recorded" << writer.framesWritten() << "frames," << writer.framesDropped()
	                    << "dropped," << (writer.gpu() ? "GPU path" : "software path");

	return writer.finalize(error);
}

// --- controller (GUI thread) -------------------------------------------------------------------

RecorderController* RecorderController::instance() {
	static auto* instance = new RecorderController(); // NOLINT
	return instance;
}

RecorderController::RecorderController() {
	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this, &RecorderController::shutdown);
	}
}

QString RecorderController::path() const { return this->job ? this->job->request.path : QString(); }

bool RecorderController::available() { return this->unavailableReason().isEmpty(); }

QString RecorderController::unavailableReason() {
	if (!this->checked) {
		this->checked = true;
		this->mUnavailableReason = checkAvailability();
		if (!this->mUnavailableReason.isEmpty()) {
			qCInfo(logRecorder) << "Native screen recording unavailable:" << this->mUnavailableReason;
		}
	}

	return this->mUnavailableReason;
}

bool RecorderController::start(const RecordingRequest& request, QString* error) {
	if (this->job) {
		*error = "already recording";
		return false;
	}

	if (!this->available()) {
		*error = this->mUnavailableReason;
		return false;
	}

	auto job = std::make_shared<RecordingJob>(request);
	auto* thread = QThread::create([job] { job->run(); });
	thread->setObjectName("quickshell-recorder");
	QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);

	this->job = job;
	this->thread = thread;
	thread->start();

	emit this->recordingChanged();
	return true;
}

void RecorderController::stop() {
	if (this->job) this->job->requestStop();
}

void RecorderController::onJobStarted(RecordingJob* job) {
	if (job != this->job.get()) return;
	emit this->started(job->request.path);
}

void RecorderController::onJobSoundUnavailable(RecordingJob* job, const QString& reason) {
	if (job != this->job.get()) return;
	emit this->soundUnavailable(reason);
}

void RecorderController::onJobDone(RecordingJob* job, bool ok, const QString& reason) {
	if (job != this->job.get()) return;

	auto path = job->request.path;
	this->job = nullptr;
	this->thread = nullptr; // deletes itself once finished

	emit this->recordingChanged();
	if (ok) emit this->finished(path);
	else emit this->failed(reason);
}

void RecorderController::shutdown() {
	if (!this->job || this->thread == nullptr) return;

	this->job->requestStop();
	if (!this->thread->wait(QDeadlineTimer(SHUTDOWN_WAIT_MS))) {
		qCWarning(logRecorder) << "The recording didn't finish before exit; the file may be broken.";
	}
}

} // namespace qs::windows::recorder
