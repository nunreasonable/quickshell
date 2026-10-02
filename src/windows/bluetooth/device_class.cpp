#include "device_class.hpp"

#include <qstring.h>
#include <qtypes.h>

namespace qs::bluetooth {

QString iconForClassOfDevice(quint32 cod) {
	// Bits 12-8: major device class, bits 7-2: minor device class (meaning depends on the major).
	const auto major = (cod >> 8) & 0x1f;
	const auto minor = (cod >> 2) & 0x3f;

	switch (major) {
	case 0x01: return QStringLiteral("computer");
	case 0x02:
		// 0x04: wired modem / voice gateway; everything else is some kind of phone.
		return minor == 0x04 ? QStringLiteral("modem") : QStringLiteral("phone");
	case 0x03: return QStringLiteral("network-wireless"); // LAN / network access point
	case 0x04: // audio / video
		switch (minor) {
		case 0x01: // wearable headset
		case 0x02: // hands-free
			return QStringLiteral("audio-headset");
		case 0x06: return QStringLiteral("audio-headphones");
		case 0x04: return QStringLiteral("audio-input-microphone");
		case 0x0b: // VCR
		case 0x0c: // video camera
		case 0x0d: // camcorder
			return QStringLiteral("camera-video");
		case 0x0e: // video monitor
		case 0x0f: // video display and loudspeaker
			return QStringLiteral("video-display");
		case 0x10: return QStringLiteral("camera-web"); // video conferencing
		case 0x12: return QStringLiteral("input-gaming");
		default: return QStringLiteral("audio-card"); // loudspeaker, portable/car/HiFi audio, ...
		}
	case 0x05: // peripheral: bits 7-6 keyboard/pointing, bits 5-2 the device type
		switch (minor & 0x30) {
		case 0x10: return QStringLiteral("input-keyboard");
		case 0x20: return QStringLiteral("input-mouse");
		case 0x30: return QStringLiteral("input-keyboard"); // combo keyboard + pointing device
		default: break;
		}
		switch (minor & 0x0f) {
		case 0x01: // joystick
		case 0x02: // gamepad
			return QStringLiteral("input-gaming");
		case 0x05: // digitizer tablet
		case 0x07: // digital pen
			return QStringLiteral("input-tablet");
		default: return {};
		}
	case 0x06: // imaging: independent flags in bits 7-4
		if (minor & 0x20) return QStringLiteral("printer");
		if (minor & 0x10) return QStringLiteral("scanner");
		if (minor & 0x08) return QStringLiteral("camera-photo");
		if (minor & 0x04) return QStringLiteral("video-display");
		return {};
	case 0x08: return QStringLiteral("input-gaming"); // toy
	default: return {};
	}
}

QString iconForAppearance(quint16 appearance) {
	const auto category = appearance >> 6;
	const auto subcategory = appearance & 0x3f;

	switch (category) {
	case 0x001: return QStringLiteral("phone");
	case 0x002: return QStringLiteral("computer");
	case 0x005: return QStringLiteral("video-display");
	case 0x00a: return QStringLiteral("multimedia-player");
	case 0x00b: return QStringLiteral("scanner"); // barcode scanner
	case 0x00f: // human interface device
		switch (subcategory) {
		case 0x01: return QStringLiteral("input-keyboard");
		case 0x02: return QStringLiteral("input-mouse");
		case 0x03: // joystick
		case 0x04: // gamepad
			return QStringLiteral("input-gaming");
		case 0x05: // digitizer tablet
		case 0x07: // digital pen
			return QStringLiteral("input-tablet");
		case 0x08: return QStringLiteral("scanner");
		case 0x09: return QStringLiteral("input-touchpad");
		default: return {};
		}
	case 0x021: return QStringLiteral("audio-speakers"); // audio sink
	case 0x022: return QStringLiteral("audio-input-microphone"); // audio source
	case 0x025: // wearable audio device: earbud, headset, headphones, neck band
		return subcategory == 0x03 ? QStringLiteral("audio-headphones") : QStringLiteral("audio-headset");
	case 0x029: return QStringLiteral("audio-headphones"); // hearing aid
	case 0x02a: return QStringLiteral("input-gaming");
	default: return {};
	}
}

} // namespace qs::bluetooth
