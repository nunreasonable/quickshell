#pragma once

#include <optional>

#include <qlist.h>
#include <qrect.h>
#include <qregion.h>
#include <qtypes.h>

class QQuickItem;
class QQuickWindow;

namespace qs::windows {

// Where to blur behind a panel, derived from its item tree. Kept free of Windows APIs so it can
// be tested on any platform.
//
// Hyprland's ignore_alpha blurs behind the pixels of a layer more opaque than a threshold. Here
// the Rectangle items of the window are the pixels: a rectangle gets blur behind it when what
// shows through there (its fill, opacity included, over the rectangles it is drawn on) is more
// opaque than the threshold and not fully opaque. The walk stops at the first such rectangle on
// each branch (its children are drawn over the same blur) and at fully opaque ones (nothing
// behind them can be seen). Text, images, shaders and canvases are not considered, which is the
// approximation: they don't get blur of their own like they would on Hyprland.

// A rounded rectangle to blur behind, in physical pixels relative to the window, cut to `clip`
// (the intersection of its ancestors' clips and the window).
struct BlurShape {
	QRectF rect;
	qreal radius = 0;
	QRectF clip;

	[[nodiscard]] bool fuzzyEquals(const BlurShape& other) const;
};

struct BlurShapeQuery {
	// Unset: the whole surface (the mask, or the window) is blurred behind.
	std::optional<qreal> ignoreAlpha;
	// The input mask in window coordinates. Only shapes touching it are kept, and an empty mask
	// (a fully click-through window) gets no blur.
	QRegion mask;
	bool hasMask = false;
	// Physical pixels per logical pixel.
	qreal dpr = 1;
};

struct BlurShapeResult {
	QList<BlurShape> shapes;
	// the item walk stopped early (huge item tree)
	bool truncated = false;
};

[[nodiscard]] BlurShapeResult collectBlurShapes(QQuickWindow* window, const BlurShapeQuery& query);

} // namespace qs::windows
