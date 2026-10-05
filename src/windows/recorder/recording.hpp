#pragma once

#include <memory>

#include <qobject.h>
#include <qrect.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtmetamacros.h>

namespace qs::windows::recorder {

class RecordingJob;

struct RecordingRequest {
	QRect region; // physical pixels, virtual desktop coordinates
	bool sound = false;
	bool cursor = true;
	int fps = 30;
	QString path;
};

///! Process wide owner of the one recording that may run at a time.
/// Lives on the GUI thread and outlives QML engine reloads, so a recording started before a
/// config reload can still be stopped after it. Each recording runs on a thread of its own
/// (RecordingJob): an MTA apartment with its own D3D11 device, the capture sessions, the
/// encoder and, with sound, a WASAPI loopback thread. stop() only signals it; the file is
/// finalized there and `finished`/`failed` arrive when it's done. At exit the GUI thread waits
/// for that, so quitting the shell mid-recording still leaves a playable file.
class RecorderController: public QObject {
	Q_OBJECT;

public:
	static RecorderController* instance();

	[[nodiscard]] bool recording() const { return this->job != nullptr; }
	[[nodiscard]] QString path() const;

	// Whether native recording can work here: Windows 10 2004+, Media Foundation present (it
	// isn't on N editions without the Media Feature Pack) with an H.264 encoder. Checked once.
	[[nodiscard]] bool available();
	[[nodiscard]] QString unavailableReason();

	// Starts recording; false (with `error`) if it can't even begin. Later failures (capture
	// or encoder errors) arrive as `failed`.
	bool start(const RecordingRequest& request, QString* error);
	void stop();

signals:
	void recordingChanged();
	void started(const QString& path);
	void finished(const QString& path);
	void failed(const QString& reason);
	void soundUnavailable(const QString& reason);

private:
	explicit RecorderController();
	~RecorderController() override = default;
	Q_DISABLE_COPY_MOVE(RecorderController);

	friend class RecordingJob;
	void onJobStarted(RecordingJob* job);
	void onJobSoundUnavailable(RecordingJob* job, const QString& reason);
	void onJobDone(RecordingJob* job, bool ok, const QString& reason);
	void shutdown();

	std::shared_ptr<RecordingJob> job;
	QThread* thread = nullptr;
	bool checked = false;
	QString mUnavailableReason;
};

} // namespace qs::windows::recorder
