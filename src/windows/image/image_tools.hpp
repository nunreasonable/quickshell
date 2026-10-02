#pragma once

#include <qobject.h>
#include <qsize.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qvariant.h>

namespace qs::windows::image {

///! QImage ports of ii's wallpaper analysis Python scripts (scripts/images/least_busy_region.py,
/// scripts/images/text_color.py, scripts/colors/scheme_for_image.py), for the background
/// clock/widget placement and the "auto" Material You scheme on Windows, where the Linux
/// *-venv.sh wrappers (a bash script activating a Python venv) have nothing to run.
///
/// These are synchronous: every input is already screen-sized or smaller by the time ii calls
/// them, so even the busiest path (the least-busy-region sliding window, done via a summed-area
/// table) is sub-millisecond work, not worth a worker thread or an async signal.
class ImageTools: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit ImageTools(QObject* parent = nullptr): QObject(parent) {}

	/// Port of least_busy_region.py's default mode (find_least_busy_region +
	/// get_dominant_color). Scales+center-crops `imagePath` to `screenWidth`x`screenHeight`
	/// (like the wallpaper itself, "fill" mode) then slides a `width`x`height` window over it
	/// (at least `horizontalPadding`/`verticalPadding` from each edge) to find the lowest- (or
	/// with `busiest`, highest-) variance region.
	///
	/// Returns a map with `center_x`, `center_y`, `width`, `height`, `variance` and
	/// `dominant_color` (`"#rrggbb"`), in the same shape least_busy_region.py prints as JSON -
	/// or `{"error": ...}` if `imagePath` couldn't be loaded.
	///
	/// `dominant_color` is the mean of the region's non-near-black pixels rather than
	/// least_busy_region.py's 3-means clustering: a deliberate simplification (this is a text
	/// contrast heuristic, not a design requirement), noted here for anyone diffing behavior
	/// against the Linux side.
	Q_INVOKABLE static QVariantMap leastBusyRegion(
	    const QString& imagePath,
	    int width,
	    int height,
	    int screenWidth,
	    int screenHeight,
	    int horizontalPadding,
	    int verticalPadding,
	    bool busiest
	);

	/// Port of text_color.py: samples `imagePath`'s four corners as the background color,
	/// then takes the median of the pixels furthest (95th percentile Euclidean distance) from
	/// it as the text color. Returns `{"background": "#rrggbb", "text": "#rrggbb"}`, or
	/// `{"error": ...}` if the image couldn't be loaded.
	Q_INVOKABLE static QVariantMap textColorFromImage(const QString& imagePath);

	/// Port of scheme_for_image.py: Hasler/Süsstrunk colorfulness of `imagePath` (resized to
	/// fit 128px), mapped to `"scheme-neutral"` (colorfulness < 40) or `"scheme-tonal-spot"` -
	/// matugen's two safe defaults for "auto" ii palette type. Returns `"scheme-tonal-spot"`
	/// if `imagePath` couldn't be loaded, matching the Python script's fallback.
	Q_INVOKABLE static QString schemeForImage(const QString& imagePath);

	/// `magick identify -format "%w %h"`: the image's pixel size (EXIF orientation applied) as
	/// `{width, height}`, read from the header only. Zero size if it can't be read.
	Q_INVOKABLE static QSize imageSize(const QString& imagePath);
};

} // namespace qs::windows::image
