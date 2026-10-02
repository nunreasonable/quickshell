#pragma once

#include <qhash.h>
#include <qobject.h>
#include <qstring.h>
#include <qt_windows.h>

namespace qs::windows::sys {

// Owner of every screen's gamma ramp, shared by NightLight (a color temperature for all screens)
// and Brightness (software dimming of a screen that has no DDC/CI or WMI control). Both end up in
// one ramp per screen, so turning one off doesn't undo the other. The original ramps are put back
// once nothing is applied anymore and when the process exits.
//
// Windows refuses ramps too far from linear (very dark or very warm ones) unless an
// administrator widened the limit in the registry; usually nothing darker than half brightness.
// So software brightness maps 0..1 onto the range a screen accepts (found once per screen), and a
// ramp that's still refused (night light adds its own darkening) is brightened in small steps.
class GammaController: public QObject {
	Q_OBJECT;

public:
	static GammaController* instance();

	// 0 turns the color temperature off.
	void setTemperature(int kelvin);
	// 0 is as dark as the screen allows, 1 turns dimming off.
	void setBrightness(const QString& screenName, qreal brightness);

	void restoreAll();

private:
	explicit GammaController(QObject* parent);

	struct Ramp {
		WORD values[3][256]; // NOLINT
	};

	bool apply(const QString& screenName, qreal brightness);
	qreal floorFor(const QString& screenName);

	int mKelvin = 0;
	QHash<QString, qreal> brightness; // ramp factor, keyed by QScreen::name()
	QHash<QString, qreal> floors;     // darkest accepted factor
	QHash<QString, Ramp> originals;
};

} // namespace qs::windows::sys
