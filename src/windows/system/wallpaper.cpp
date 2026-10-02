#include "wallpaper.hpp"

#include <qcolor.h>
#include <qcoreapplication.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qloggingcategory.h>
#include <qstring.h>

#include <qt_windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace qs::windows::sys {

namespace {
Q_LOGGING_CATEGORY(logWallpaper, "quickshell.windows.wallpaper", QtWarningMsg);

const wchar_t* const kPersonalizeKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
const wchar_t* const kDwmKey = L"Software\\Microsoft\\Windows\\DWM";

void broadcastSettingChange(const wchar_t* setting) {
	DWORD_PTR result = 0;
	// Same message Settings sends after flipping its light/dark or accent color toggles;
	// without it, only newly started apps would notice the registry change.
	SendMessageTimeoutW(
	    HWND_BROADCAST,
	    WM_SETTINGCHANGE,
	    0,
	    reinterpret_cast<LPARAM>(setting),
	    SMTO_ABORTIFHUNG,
	    2000,
	    &result
	);
}

bool readDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& out) {
	HKEY key = nullptr;
	if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
	DWORD value = 0;
	DWORD size = sizeof(value);
	DWORD type = 0;
	auto ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
	RegCloseKey(key);
	if (ok != ERROR_SUCCESS || type != REG_DWORD) return false;
	out = value;
	return true;
}

bool writeDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value) {
	HKEY key = nullptr;
	if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
	    != ERROR_SUCCESS)
	{
		return false;
	}

	auto ok =
	    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));

	RegCloseKey(key);
	return ok == ERROR_SUCCESS;
}

// Parses "#RRGGBB" / "RRGGBB". Returns false (leaving `out` untouched) if malformed.
bool parseHexColor(const QString& hex, QColor& out) {
	auto trimmed = hex.startsWith(u'#') ? hex.mid(1) : hex;
	if (trimmed.length() != 6) return false;

	bool ok = false;
	auto value = trimmed.toUInt(&ok, 16);
	if (!ok) return false;

	out = QColor(
	    static_cast<int>((value >> 16) & 0xFF),
	    static_cast<int>((value >> 8) & 0xFF),
	    static_cast<int>(value & 0xFF)
	);

	return true;
}

} // namespace

QString Wallpaper::currentWallpaper() {
	ComPtr<IDesktopWallpaper> wallpaper;
	auto hr =
	    CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper));

	if (SUCCEEDED(hr) && wallpaper) {
		LPWSTR path = nullptr;
		// Monitor id null = "the first/primary monitor", which is what SPI_GETDESKWALLPAPER
		// would give on single-wallpaper setups too; ii only needs *a* representative image
		// to seed its background with on first run, not a per-monitor list.
		hr = wallpaper->GetWallpaper(nullptr, &path);
		if (SUCCEEDED(hr) && path != nullptr) {
			auto result = QString::fromWCharArray(path);
			CoTaskMemFree(path);
			if (!result.isEmpty()) return result;
		}
	} else {
		qCDebug(logWallpaper) << "IDesktopWallpaper unavailable for read:" << Qt::hex << hr;
	}

	wchar_t buf[MAX_PATH] = {};
	if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, buf, 0)) {
		return QString::fromWCharArray(buf);
	}

	return {};
}

bool Wallpaper::setWallpaper(const QString& path) {
	if (path.isEmpty()) return false;

	ComPtr<IDesktopWallpaper> wallpaper;
	auto hr =
	    CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper));

	if (FAILED(hr) || !wallpaper) {
		qCWarning(logWallpaper) << "IDesktopWallpaper unavailable:" << Qt::hex << hr;
		return false;
	}

	// DWPOS_FILL matches Hyprland's `wallpaper` default on the Linux side (crop to fill,
	// no letterboxing).
	wallpaper->SetPosition(DWPOS_FILL);

	// IDesktopWallpaper stores the path as given, and Windows' own UI expects backslashes.
	auto wpath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
	bool anyOk = false;

	UINT count = 0;
	if (SUCCEEDED(wallpaper->GetMonitorDevicePathCount(&count)) && count > 0) {
		for (UINT i = 0; i < count; i++) {
			LPWSTR monitorId = nullptr;
			if (FAILED(wallpaper->GetMonitorDevicePathAt(i, &monitorId)) || monitorId == nullptr) {
				continue;
			}

			auto setHr = wallpaper->SetWallpaper(monitorId, wpath.c_str());
			if (SUCCEEDED(setHr)) {
				anyOk = true;
			} else {
				qCWarning(logWallpaper) << "SetWallpaper failed for monitor" << i << ":" << Qt::hex
				                        << setHr;
			}

			CoTaskMemFree(monitorId);
		}
	}

	if (!anyOk) {
		// No per-monitor device paths (or all of them failed): a null monitor id targets
		// every monitor at once.
		auto setHr = wallpaper->SetWallpaper(nullptr, wpath.c_str());
		anyOk = SUCCEEDED(setHr);
		if (!anyOk) qCWarning(logWallpaper) << "SetWallpaper(all monitors) failed:" << Qt::hex << setHr;
	}

	return anyOk;
}

bool Wallpaper::isDarkMode() {
	DWORD value = 1;
	if (!readDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", value)) return false;
	return value == 0;
}

void Wallpaper::setDarkMode(bool dark) {
	DWORD value = dark ? 0 : 1;
	auto ok1 = writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", value);
	auto ok2 = writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme", value);

	if (!ok1 || !ok2) qCWarning(logWallpaper) << "failed writing theme registry values";

	broadcastSettingChange(L"ImmersiveColorSet");
}

bool Wallpaper::setAccentColor(const QString& hex) {
	QColor color;
	if (!parseHexColor(hex, color)) {
		qCWarning(logWallpaper) << "setAccentColor: not a hex color:" << hex;
		return false;
	}

	// AccentColor is 0xAABBGGRR (note: BGR, not RGB) with full alpha - used for title
	// bars/borders when "Show accent color on title bars" is on.
	DWORD accentColor = 0xFF000000u | (static_cast<DWORD>(color.blue()) << 16)
	                   | (static_cast<DWORD>(color.green()) << 8) | static_cast<DWORD>(color.red());

	// ColorizationColor is 0xAARRGGBB with the alpha DWM uses to blend the colorization atop
	// the background (0xC4, the same value Windows itself writes for an accent set from a
	// picture via Settings > Personalization > Colors).
	DWORD colorizationColor = 0xC4000000u | (static_cast<DWORD>(color.red()) << 16)
	                         | (static_cast<DWORD>(color.green()) << 8) | static_cast<DWORD>(color.blue());

	auto ok1 = writeDword(HKEY_CURRENT_USER, kDwmKey, L"AccentColor", accentColor);
	auto ok2 = writeDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationColor", colorizationColor);
	auto ok3 = writeDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationAfterglow", colorizationColor);

	if (!ok1 || !ok2 || !ok3) {
		qCWarning(logWallpaper) << "failed writing accent color registry values";
	}

	broadcastSettingChange(L"ImmersiveColorSet");
	return ok1 && ok2;
}

QString Wallpaper::matugenPath() {
	auto path = QCoreApplication::applicationDirPath() + QStringLiteral("/matugen.exe");
	return QFileInfo::exists(path) ? path : QString();
}

} // namespace qs::windows::sys
