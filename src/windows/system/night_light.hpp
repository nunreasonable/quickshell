#pragma once

#include <qhash.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qt_windows.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <vector>

namespace qs::windows::sys {

///! Warm-tint night light via `SetDeviceGammaRamp`, for Hyprsunset.qml (replaces hyprsunset).
/// There is no DWM/compositor-level color filter exposed to apps, so this adjusts the gamma
/// ramp of every monitor's device context directly -- the same trick f.lux/Redshift use on
/// Windows. The original ramp is restored on @@disable and when the process exits.
class NightLight: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit NightLight(QObject* parent = nullptr);
	~NightLight() override;
	Q_DISABLE_COPY_MOVE(NightLight);

	/// Applies a warm gamma ramp for `colorTemperatureKelvin` (lower = warmer) to every monitor.
	Q_INVOKABLE void enable(int colorTemperatureKelvin);
	/// Restores every monitor's original gamma ramp.
	Q_INVOKABLE void disable();

private:
	struct OriginalRamp {
		WORD ramp[3][256]; // NOLINT
	};

	void restoreAll();

	bool mEnabled = false;
	QHash<QString, OriginalRamp> originalRamps; // keyed by QScreen::name()
};

} // namespace qs::windows::sys
