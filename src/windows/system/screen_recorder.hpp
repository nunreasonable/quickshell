#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

///! Screen region recording to H.264/AAC .mp4 (replaces wf-recorder, and ffmpeg on Windows).
/// Captures the monitors under the region with Windows.Graphics.Capture (cursor included, no
/// yellow border where Windows allows turning it off: Windows 11), encodes with Media
/// Foundation (a hardware encoder when the GPU has one) at a constant 30 fps, and optionally
/// mixes in what the speakers play through WASAPI loopback.
///
/// One recording runs at a time per process; it survives config reloads and is finalized
/// when the shell exits.
class ScreenRecorder: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	/// True from a successful @@start() until @@finished or @@failed.
	Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged);
	/// The file being recorded, empty when not recording.
	Q_PROPERTY(QString path READ path NOTIFY recordingChanged);
	/// Whether native recording works on this system (Windows 10 2004+, Media Foundation with
	/// an H.264 encoder; Windows N editions lack Media Foundation without the Media Feature Pack).
	Q_PROPERTY(bool available READ available CONSTANT);
	/// Why @@available is false; empty otherwise.
	Q_PROPERTY(QString unavailableReason READ unavailableReason CONSTANT);

public:
	explicit ScreenRecorder(QObject* parent = nullptr);

	/// Starts recording the rectangle `(x, y, width, height)`, in physical pixels of the virtual
	/// desktop (the primary monitor's top left is 0,0; HyprlandMonitor.x/y plus the region's
	/// offset scaled by the monitor's scale). Odd sizes lose their last row/column (H.264 needs
	/// even ones). `sound` adds what the default output device plays. The file goes to
	/// `saveDir` (default: the Videos folder) as `recording_<yyyy-MM-dd_HH.mm.ss>.mp4`.
	///
	/// Returns false (and emits @@failed) if recording can't start at all.
	Q_INVOKABLE bool start(
	    int x,
	    int y,
	    int width,
	    int height,
	    bool sound = false,
	    const QString& saveDir = QString()
	);
	/// Records a whole screen. `screenName` is a @@Quickshell.ShellScreen's `name`.
	Q_INVOKABLE bool
	startScreen(const QString& screenName, bool sound = false, const QString& saveDir = QString());
	/// Stops the recording; the file is finished in the background and @@finished follows.
	Q_INVOKABLE void stop();

	[[nodiscard]] bool recording() const;
	[[nodiscard]] QString path() const;
	[[nodiscard]] bool available() const;
	[[nodiscard]] QString unavailableReason() const;

signals:
	void recordingChanged();
	/// The first frame is being recorded to `path`.
	void started(const QString& path);
	/// The recording was stopped and `path` is complete.
	void finished(const QString& path);
	/// The recording couldn't start or broke off; no file is left behind.
	void failed(const QString& reason);
	/// Sound was asked for but can't be recorded (no output device, no AAC encoder); the
	/// recording goes on without it.
	void soundUnavailable(const QString& reason);
};

} // namespace qs::windows::sys
