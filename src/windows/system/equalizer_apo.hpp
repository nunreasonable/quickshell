#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class EqualizerApo: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(bool installed READ installed NOTIFY changed);
	Q_PROPERTY(QString installPath READ installPath NOTIFY changed);
	Q_PROPERTY(QString configPath READ configPath NOTIFY changed);
	Q_PROPERTY(QString configFilePath READ configFilePath NOTIFY changed);
	Q_PROPERTY(QString includeFilePath READ includeFilePath NOTIFY changed);
	Q_PROPERTY(bool configWritable READ configWritable NOTIFY changed);
	Q_PROPERTY(bool hooked READ hooked NOTIFY changed);
	Q_PROPERTY(bool activeOnDefaultDevice READ activeOnDefaultDevice NOTIFY changed);
	Q_PROPERTY(QString defaultDeviceId READ defaultDeviceId NOTIFY changed);
	Q_PROPERTY(bool ready READ ready NOTIFY changed);
	// clang-format on
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit EqualizerApo(QObject* parent = nullptr);

	[[nodiscard]] bool installed() const { return this->mInstalled; }
	[[nodiscard]] QString installPath() const { return this->mInstallPath; }
	[[nodiscard]] QString configPath() const { return this->mConfigPath; }
	[[nodiscard]] QString configFilePath() const;
	[[nodiscard]] QString includeFilePath() const;
	[[nodiscard]] bool configWritable() const { return this->mConfigWritable; }
	[[nodiscard]] bool hooked() const { return this->mHooked; }
	[[nodiscard]] bool activeOnDefaultDevice() const { return this->mActiveOnDevice; }
	[[nodiscard]] QString defaultDeviceId() const { return this->mDefaultDeviceId; }
	[[nodiscard]] bool ready() const { return this->mInstalled && this->mActiveOnDevice; }

	// Re-runs every registry/filesystem probe below and emits changed() once.
	Q_INVOKABLE void refresh();

	// Called from QML whenever Audio.sink (Pipewire's defaultAudioSink) changes,
	// so activeOnDefaultDevice/ready follow the system's actual default output.
	Q_INVOKABLE void setDefaultDeviceId(const QString& id);

	// Atomically (over)writes ii's own include file (includeFilePath) with the
	// given Equalizer APO config text (Preamp/Filter/GraphicEQ lines). Never
	// touches config.txt itself.
	Q_INVOKABLE bool writeCurve(const QString& content) const;

	// Appends the "Include: <includeFilePath>" line to config.txt directly.
	// Only call this after configWritable is true - returns false otherwise
	// without writing anything.
	Q_INVOKABLE bool hookDirectly();

	// Fire-and-forget: spawns one elevated step (a UAC prompt) that appends the
	// same Include line via a tiny generated PowerShell script. Callers should
	// poll refresh()/hooked afterwards (e.g. from a short-lived QML Timer) -
	// there is no synchronous completion signal, since the elevated process is
	// detached and may wait on the user's UAC response.
	Q_INVOKABLE void hookElevated();

signals:
	void changed();

private:
	void detectInstall();
	void detectWritable();
	void detectHooked();
	void detectActiveOnDevice();

	bool mInstalled = false;
	QString mInstallPath;
	QString mConfigPath;
	bool mConfigWritable = false;
	bool mHooked = false;
	bool mActiveOnDevice = false;
	QString mDefaultDeviceId;
};

} // namespace qs::windows::sys
