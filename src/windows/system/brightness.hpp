#pragma once

#include <qhash.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows::sys {

///! Per-screen display brightness. External monitors go through DDC/CI (dxva2); internal
/// panels go through the `root\wmi` `WmiMonitorBrightness`/`WmiMonitorBrightnessMethods` classes.
/// Both are slow (DDC is an I2C round trip, WMI is COM), so every operation runs on a worker
/// thread and reports back asynchronously.
///
/// Screens are identified by `QScreen::name()` (ii's existing `ShellScreen.name`), matching how
/// the old ddcutil/brightnessctl-based QML service already keyed its per-monitor state.
class Brightness: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Brightness(QObject* parent = nullptr): QObject(parent) {}

	/// Asynchronously determines whether `screenName` is DDC or WMI controlled (or neither) and
	/// its current brightness. Emits @@queried when done.
	Q_INVOKABLE void query(const QString& screenName);

	/// Asynchronously sets `screenName`'s brightness to `value` (0..1). `isDdc` must be whatever
	/// @@queried last reported for this screen.
	Q_INVOKABLE void setBrightness(const QString& screenName, bool isDdc, qreal value);

signals:
	/// `available` is false if neither DDC nor a WMI brightness instance could be matched to
	/// this screen (desktop monitor with no DDC/CI support, started with it disabled, etc).
	void queried(const QString& screenName, bool available, bool isDdc, qreal brightness);
	void brightnessSetFinished(const QString& screenName, bool ok);

private:
	// Cache of how a screen was last resolved: -1 means DDC/CI, >=0 is a WMI
	// WmiMonitorBrightness instance index. Internal (non-DDC) panels can't be matched to a
	// QScreen by name, so the first screen that falls back to WMI claims the next unclaimed
	// instance in enumeration order -- good enough for the common single-internal-panel case;
	// see brightness.cpp.
	QHash<QString, int> screenRoute;
	int nextWmiInstanceIndex = 0;
};

} // namespace qs::windows::sys
