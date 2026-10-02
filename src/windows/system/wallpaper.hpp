#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

///! Desktop wallpaper plus the system light/dark mode and (best effort) accent color, via
/// IDesktopWallpaper and the Explorer/DWM "Personalize"/"Accent" registry keys.
///
/// On Linux this is scripts/colors/switchwall.sh's OS-integration half: it still calls out to
/// matugen.exe for the actual Material You palette (colors.json), this singleton only drives
/// the Windows-side state matugen has no access to (the desktop wallpaper image, Settings'
/// light/dark toggle, and optionally the taskbar/title bar accent color).
class Wallpaper: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Wallpaper(QObject* parent = nullptr): QObject(parent) {}

	/// The current desktop wallpaper path (IDesktopWallpaper::GetWallpaper for monitor 0,
	/// falling back to SPI_GETDESKWALLPAPER), or empty on failure. Read-only - used on first
	/// run so ii's background shows whatever the user already had set instead of a blank
	/// screen, without ii ever having called setWallpaper() itself.
	Q_INVOKABLE static QString currentWallpaper();

	/// Sets `path` (an absolute path to a static image) as the wallpaper on every monitor,
	/// filling each one (DWPOS_FILL, matching Hyprland's default `fill` mode on the Linux
	/// side). Returns false on failure. Callers must not pass a video path - Windows has no
	/// equivalent to mpvpaper, so video wallpapers stay unsupported here.
	Q_INVOKABLE static bool setWallpaper(const QString& path);

	/// True if Apps are currently in dark mode (`AppsUseLightTheme` == 0), read straight from
	/// the registry.
	Q_INVOKABLE static bool isDarkMode();

	/// Sets Explorer's "app mode" and "system mode" (HKCU
	/// `...\Themes\Personalize\AppsUseLightTheme` / `SystemUsesLightTheme`) and broadcasts
	/// `WM_SETTINGCHANGE("ImmersiveColorSet")` so running apps and Explorer pick it up
	/// immediately - the same signal the Settings app sends when its toggle is flipped.
	Q_INVOKABLE static void setDarkMode(bool dark);

	/// Best-effort: sets the Windows accent color (DWM `ColorizationColor` / `AccentColor`)
	/// from a Material You primary color (`#RRGGBB`), and broadcasts the same
	/// `ImmersiveColorSet` change so the taskbar/title bars re-read it. Returns false (and
	/// changes nothing) if `hex` doesn't parse.
	///
	/// Deliberately does *not* touch `...\Explorer\Accent\AccentColorMenu` /
	/// `AccentPalette`: those hold a 15-shade palette (Explorer start menu/taskbar tints)
	/// that Windows derives from the base color with an unpublished algorithm, and writing a
	/// guessed palette there risks a visibly inconsistent accent across Explorer surfaces for
	/// no clean way to verify it. `AccentColor`/`ColorizationColor` are plain single values
	/// with a well understood format, so those are the only ones set.
	Q_INVOKABLE static bool setAccentColor(const QString& hex);

	/// Full path to `matugen.exe` next to the running executable (tools/deploy-ii.sh stages
	/// it there, alongside qs.exe/qsw.exe), or empty if it isn't there - e.g. a dev build that
	/// hasn't run deploy-ii.sh. QML call sites should treat empty as "wallpaper theming
	/// unavailable" rather than trying to spawn a bare `matugen.exe` and hoping it's on PATH.
	Q_INVOKABLE static QString matugenPath();
};

} // namespace qs::windows::sys
