#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

///! Warm-tint night light via the screens' gamma ramps, for Hyprsunset.qml (replaces hyprsunset).
/// There is no DWM/compositor-level color filter exposed to apps, so this adjusts the gamma
/// ramp of every monitor's device context directly -- the same trick f.lux/Redshift use on
/// Windows. GammaController combines it with Brightness's software dimming; the original ramps
/// come back on @@disable (unless dimming still needs them) and when the process exits.
class NightLight: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit NightLight(QObject* parent = nullptr): QObject(parent) {}

	/// Applies a warm gamma ramp for `colorTemperatureKelvin` (lower = warmer) to every monitor.
	Q_INVOKABLE void enable(int colorTemperatureKelvin);
	/// Takes the warm tint off every monitor.
	Q_INVOKABLE void disable();
};

} // namespace qs::windows::sys
