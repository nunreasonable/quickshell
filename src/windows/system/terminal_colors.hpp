#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qvariant.h>

namespace qs::windows::sys {

///! Terminal color harmonization from ii's generate_colors_material.py, natively.
/// On Linux, switchwall.sh runs scripts/colors/generate_colors_material.py, which (besides the
/// material scheme that matugen also makes) turns scripts/colors/terminal/scheme-base.json into
/// term0..term15 tinted towards the wallpaper. Windows has no Python to run it with, so this is
/// that script's terminal part on material-color-utilities' C++ HCT (third_party/material_color,
/// adjusted to be bit-identical to the materialyoucolor package the script uses).
///
/// Pure and synchronous. Colors are strings QColor parses ("#RRGGBB" and friends; alpha is
/// ignored) or QML colors.
class TerminalColors: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit TerminalColors(QObject* parent = nullptr): QObject(parent) {}

	/// The script's term colors, as switchwall.sh runs it (--blend_bg_fg): `baseScheme` is the
	/// `dark` or `light` object of scheme-base.json ({"term0": "#282828", ...}); the result has
	/// the same keys, each "#RRGGBB" (uppercase, like the script's argb_to_hex). term0 is
	/// surfaceContainerLow and term15 onSurface, boosted; the rest are their base colors
	/// harmonized towards primaryKeyColor (the material primary_paletteKeyColor) by `harmony`,
	/// at most `harmonizeThreshold` degrees, with their tone scaled by 1 +/- fgBoost (+ in dark
	/// mode). With `monochrome` every color is passed through as given, as is any color whose
	/// inputs don't parse.
	Q_INVOKABLE static QVariantMap generate(
	    const QVariantMap& baseScheme,
	    const QString& primaryKeyColor,
	    const QString& surfaceContainerLow,
	    const QString& onSurface,
	    bool darkMode,
	    double harmony,
	    double harmonizeThreshold,
	    double fgBoost,
	    bool monochrome
	);

	/// HCT hue of `color` in degrees (0 to 360); NaN if it doesn't parse.
	Q_INVOKABLE static double hue(const QString& color);
};

} // namespace qs::windows::sys
