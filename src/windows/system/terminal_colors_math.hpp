#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Qt-free on purpose: tools/terminal-colors-test (in the ii-windows repo) builds this file with
// a plain host compiler and checks it against ii's Python generator.
namespace qs::windows::sys::terminal_colors {

/// 0xAARRGGBB. Alpha is ignored on input and 0xFF on output.
using Argb = std::uint32_t;

/// The inputs of the terminal part of ii's scripts/colors/generate_colors_material.py, as
/// switchwall.sh runs it (always with --blend_bg_fg). A color that's missing (nullopt) leaves
/// the terminal colors computed from it as they are in the base scheme.
struct TerminalInputs {
	std::optional<Argb> primaryKeyColor;     // material primary_paletteKeyColor
	std::optional<Argb> surfaceContainerLow; // material surfaceContainerLow
	std::optional<Argb> onSurface;           // material onSurface
	bool darkMode = true;
	double harmony = 0.8;              // --harmony
	double harmonizeThreshold = 100;   // --harmonize_threshold
	double fgBoost = 0.35;             // --term_fg_boost
	bool monochrome = false;           // the scheme keeps the base colors
};

/// The generator's harmonize(): rotates designColor's HCT hue towards sourceColor's by
/// `harmony` times the angle between them, but by at most `threshold` degrees.
Argb harmonize(Argb designColor, Argb sourceColor, double threshold, double harmony);

/// The generator's boost_chroma_tone(): scales HCT chroma and tone.
Argb boostChromaTone(Argb color, double chroma, double tone);

/// The generator's value for the terminal color `name` (term0..term15) whose base scheme
/// color is `base`: term0 is surfaceContainerLow and term15 onSurface, both boosted; every
/// other name is its base color harmonized towards the primary key color, its tone pushed away
/// from the background by fgBoost. nullopt means "keep the base color": monochrome, or a color
/// it needs is missing.
std::optional<Argb>
terminalColor(std::string_view name, std::optional<Argb> base, const TerminalInputs& inputs);

/// HCT hue in degrees (0 to 360).
double hctHue(Argb color);

} // namespace qs::windows::sys::terminal_colors
