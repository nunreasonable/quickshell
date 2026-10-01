#include "device.hpp"

#include <qt_windows.h>

#include <qobject.h>
#include <qstring.h>
#include <qtypes.h>

namespace qs::service::upower {

QString UPowerDeviceState::toString(UPowerDeviceState::Enum status) {
	switch (status) {
	case UPowerDeviceState::Unknown: return "Unknown";
	case UPowerDeviceState::Charging: return "Charging";
	case UPowerDeviceState::Discharging: return "Discharging";
	case UPowerDeviceState::Empty: return "Empty";
	case UPowerDeviceState::FullyCharged: return "Fully Charged";
	case UPowerDeviceState::PendingCharge: return "Pending Charge";
	case UPowerDeviceState::PendingDischarge: return "Pending Discharge";
	default: return "Invalid Status";
	}
}

QString UPowerDeviceType::toString(UPowerDeviceType::Enum type) {
	switch (type) {
	case UPowerDeviceType::Unknown: return "Unknown";
	case UPowerDeviceType::LinePower: return "Line Power";
	case UPowerDeviceType::Battery: return "Battery";
	default: return "Invalid Type";
	}
}

UPowerDevice::UPowerDevice(QObject* parent): QObject(parent) {
	this->bIsLaptopBattery.setBinding([this]() {
		return this->bType == UPowerDeviceType::Battery && this->bPowerSupply.value();
	});

	this->bHealthSupported.setBinding([this]() { return false; });
	this->bHealthPercentage.setBinding([this]() { return 100.0; });
	this->bReady = true;
}

// GetSystemPowerStatus does not expose a charge/discharge wattage or a time-to-full estimate;
// those stay at 0, matching the "no battery" shim defaults when unknown. See also BATTERY_STATUS
// (IOCTL_BATTERY_QUERY_STATUS) for a richer source if this ever needs to improve.
void UPowerDevice::updateFromSystemPowerStatus() {
	SYSTEM_POWER_STATUS status {};
	if (!GetSystemPowerStatus(&status)) {
		this->bIsPresent = false;
		this->bType = UPowerDeviceType::Unknown;
		return;
	}

	auto present = status.BatteryFlag != 128 && status.BatteryFlag != 255;

	this->bIsPresent = present;
	this->bPowerSupply = present;
	this->bType = present ? UPowerDeviceType::Battery : UPowerDeviceType::Unknown;
	this->bNativePath = present ? QStringLiteral("WindowsBattery") : QString();
	this->bModel = present ? QStringLiteral("System Battery") : QString();

	auto percent =
	    status.BatteryLifePercent == 255 ? 0.0 : qreal(status.BatteryLifePercent) / 100.0;
	this->bPercentage = present ? percent : 0.0;

	auto charging = (status.BatteryFlag & 8) != 0;
	auto onAc = status.ACLineStatus == 1;

	UPowerDeviceState::Enum state = UPowerDeviceState::Unknown;
	if (present) {
		if (charging) {
			state = UPowerDeviceState::Charging;
		} else if (onAc) {
			state = percent >= 0.99 ? UPowerDeviceState::FullyCharged : UPowerDeviceState::Charging;
		} else {
			state = UPowerDeviceState::Discharging;
		}
	}
	this->bState = state;

	auto lifeTimeUnknown = status.BatteryLifeTime == static_cast<DWORD>(-1);
	this->bTimeToEmpty =
	    (present && !onAc && !lifeTimeUnknown) ? qreal(status.BatteryLifeTime) : 0.0;
	// No time-to-full field in SYSTEM_POWER_STATUS.
	this->bTimeToFull = 0.0;

	this->bIconName = present
	    ? (charging ? QStringLiteral("battery-good-charging-symbolic")
	                : QStringLiteral("battery-good-symbolic"))
	    : QString();
}

} // namespace qs::service::upower
