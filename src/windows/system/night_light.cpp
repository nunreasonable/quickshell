#include "night_light.hpp"

#include <algorithm>
#include <cmath>

#include <qcoreapplication.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qscreen.h>
#include <qstring.h>

#include "../../core/logcat.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logNightLight, "quickshell.windows.nightlight", QtWarningMsg);

// Tanner Helland's blackbody-radiation RGB approximation (the same one most open source
// blue-light filters use), returns 0..255 per channel for `kelvin`.
void rgbForColorTemperature(int kelvin, double& red, double& green, double& blue) {
	auto temp = std::clamp(kelvin, 1000, 40000) / 100.0;

	if (temp <= 66.0) {
		red = 255.0;
	} else {
		red = 329.698727446 * std::pow(temp - 60.0, -0.1332047592);
	}

	if (temp <= 66.0) {
		green = 99.4708025861 * std::log(temp) - 161.1195681661;
	} else {
		green = 288.1221695283 * std::pow(temp - 60.0, -0.0755148492);
	}

	if (temp >= 66.0) {
		blue = 255.0;
	} else if (temp <= 19.0) {
		blue = 0.0;
	} else {
		blue = 138.5177312231 * std::log(temp - 10.0) - 305.0447927307;
	}

	red = std::clamp(red, 0.0, 255.0);
	green = std::clamp(green, 0.0, 255.0);
	blue = std::clamp(blue, 0.0, 255.0);
}

void buildRamp(double red, double green, double blue, WORD ramp[3][256]) {
	double factors[3] = {red / 255.0, green / 255.0, blue / 255.0};
	for (int channel = 0; channel < 3; channel++) {
		for (int i = 0; i < 256; i++) {
			auto value = static_cast<double>(i) * 257.0 * factors[channel];
			ramp[channel][i] = static_cast<WORD>(std::clamp(value, 0.0, 65535.0));
		}
	}
}

} // namespace

NightLight::NightLight(QObject* parent): QObject(parent) {
	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this, &NightLight::restoreAll);
	}
}

NightLight::~NightLight() { this->restoreAll(); }

void NightLight::enable(int colorTemperatureKelvin) {
	double red = 255.0;
	double green = 255.0;
	double blue = 255.0;
	rgbForColorTemperature(colorTemperatureKelvin, red, green, blue);

	WORD newRamp[3][256];
	buildRamp(red, green, blue, newRamp);

	for (auto* screen: QGuiApplication::screens()) {
		auto name = screen->name();
		auto wname = name.toStdWString();

		auto* hdc = CreateDCW(L"DISPLAY", wname.c_str(), nullptr, nullptr);
		if (hdc == nullptr) continue;

		if (!this->originalRamps.contains(name)) {
			OriginalRamp original {};
			if (GetDeviceGammaRamp(hdc, original.ramp)) {
				this->originalRamps.insert(name, original);
			}
		}

		if (!SetDeviceGammaRamp(hdc, newRamp)) {
			qCWarning(logNightLight) << "SetDeviceGammaRamp failed for" << name << ":"
			                         << GetLastError();
		}

		DeleteDC(hdc);
	}

	this->mEnabled = true;
}

void NightLight::disable() { this->restoreAll(); }

void NightLight::restoreAll() {
	if (!this->mEnabled) return;

	for (auto* screen: QGuiApplication::screens()) {
		auto name = screen->name();
		auto iter = this->originalRamps.constFind(name);
		if (iter == this->originalRamps.constEnd()) continue;

		auto wname = name.toStdWString();
		auto* hdc = CreateDCW(L"DISPLAY", wname.c_str(), nullptr, nullptr);
		if (hdc == nullptr) continue;

		SetDeviceGammaRamp(hdc, const_cast<WORD*>(&iter.value().ramp[0][0])); // NOLINT
		DeleteDC(hdc);
	}

	this->mEnabled = false;
}

} // namespace qs::windows::sys
