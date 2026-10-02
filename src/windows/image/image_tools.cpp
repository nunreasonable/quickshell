#include "image_tools.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include <qcolor.h>
#include <qimage.h>
#include <qimagereader.h>
#include <qloggingcategory.h>
#include <qpoint.h>
#include <qrect.h>
#include <qrgb.h>
#include <qsize.h>

namespace qs::windows::image {

namespace {
Q_LOGGING_CATEGORY(logImageTools, "quickshell.windows.imagetools", QtWarningMsg);

QString toHex(int r, int g, int b) {
	return QColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)).name();
}

// Scales `src` to cover `targetW`x`targetH` ("fill", like least_busy_region.py's default
// screen-mode) and center-crops to exactly that size. Matches the wallpaper's own on-screen
// framing so the region search operates on what the user actually sees.
QImage scaleAndCropToScreen(const QImage& src, int targetW, int targetH) {
	if (targetW <= 0 || targetH <= 0 || src.isNull()) return src;

	auto scaleW = static_cast<double>(targetW) / src.width();
	auto scaleH = static_cast<double>(targetH) / src.height();
	auto scale = std::max(scaleW, scaleH);

	auto newW = std::max(1, static_cast<int>(std::lround(src.width() * scale)));
	auto newH = std::max(1, static_cast<int>(std::lround(src.height() * scale)));

	auto scaled = src.scaled(newW, newH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

	auto x1 = std::max(0, (newW - targetW) / 2);
	auto y1 = std::max(0, (newH - targetH) / 2);
	auto w = std::min(targetW, newW);
	auto h = std::min(targetH, newH);

	return scaled.copy(x1, y1, w, h);
}

// Summed-area table (and its square, for a region's variance) over a single-channel double
// grid, ported from least_busy_region.py's use of cv2.integral.
struct IntegralImage {
	int w = 0;
	int h = 0;
	std::vector<double> sum;
	std::vector<double> sumSq;

	IntegralImage(const QImage& gray): w(gray.width()), h(gray.height()) {
		auto cellCount = static_cast<size_t>(w) * h;
		this->sum.assign(cellCount, 0.0);
		this->sumSq.assign(cellCount, 0.0);

		for (int y = 0; y < h; y++) {
			const auto* line = gray.constScanLine(y);
			for (int x = 0; x < w; x++) {
				double v = line[x];
				double left = x > 0 ? this->sum[idx(x - 1, y)] : 0.0;
				double up = y > 0 ? this->sum[idx(x, y - 1)] : 0.0;
				double upLeft = (x > 0 && y > 0) ? this->sum[idx(x - 1, y - 1)] : 0.0;
				this->sum[idx(x, y)] = v + left + up - upLeft;

				double leftSq = x > 0 ? this->sumSq[idx(x - 1, y)] : 0.0;
				double upSq = y > 0 ? this->sumSq[idx(x, y - 1)] : 0.0;
				double upLeftSq = (x > 0 && y > 0) ? this->sumSq[idx(x - 1, y - 1)] : 0.0;
				this->sumSq[idx(x, y)] = v * v + leftSq + upSq - upLeftSq;
			}
		}
	}

	[[nodiscard]] size_t idx(int x, int y) const { return static_cast<size_t>(y) * w + x; }

	// Inclusive region sum, identical logic to least_busy_region.py's region_sum().
	[[nodiscard]] double regionSum(const std::vector<double>& ii, int x1, int y1, int x2, int y2)
	    const {
		double total = ii[idx(x2, y2)];
		if (x1 > 0) total -= ii[idx(x1 - 1, y2)];
		if (y1 > 0) total -= ii[idx(x2, y1 - 1)];
		if (x1 > 0 && y1 > 0) total += ii[idx(x1 - 1, y1 - 1)];
		return total;
	}
};

// Mean of the region's non-near-black pixels. A simplified stand-in for
// least_busy_region.py's get_dominant_color(), which clusters the region into 3 colors with
// k-means and takes the largest cluster - this is a text/UI contrast heuristic, not a design
// requirement, so the simpler (and much cheaper) mean is close enough.
QString dominantColorOfRegion(const QImage& color, const QRect& rect) {
	auto clamped = rect.intersected(color.rect());
	if (clamped.isEmpty()) return toHex(0, 0, 0);

	long long sumR = 0, sumG = 0, sumB = 0;
	long long count = 0;

	for (int y = clamped.top(); y <= clamped.bottom(); y++) {
		for (int x = clamped.left(); x <= clamped.right(); x++) {
			auto pixel = color.pixelColor(x, y);
			if (pixel.red() <= 10 && pixel.green() <= 10 && pixel.blue() <= 10) continue;
			sumR += pixel.red();
			sumG += pixel.green();
			sumB += pixel.blue();
			count++;
		}
	}

	if (count == 0) {
		// Every pixel was near-black: fall back to the plain mean (matches the Python
		// script falling back to the unfiltered region when non_black is empty).
		for (int y = clamped.top(); y <= clamped.bottom(); y++) {
			for (int x = clamped.left(); x <= clamped.right(); x++) {
				auto pixel = color.pixelColor(x, y);
				sumR += pixel.red();
				sumG += pixel.green();
				sumB += pixel.blue();
				count++;
			}
		}
	}

	if (count == 0) return toHex(0, 0, 0);
	return toHex(
	    static_cast<int>(sumR / count),
	    static_cast<int>(sumG / count),
	    static_cast<int>(sumB / count)
	);
}

} // namespace

QVariantMap ImageTools::leastBusyRegion(
    const QString& imagePath,
    int width,
    int height,
    int screenWidth,
    int screenHeight,
    int horizontalPadding,
    int verticalPadding,
    bool busiest
) {
	if (imagePath.isEmpty()) return QVariantMap {{"error", QStringLiteral("No image")}};

	QImage original(imagePath);
	if (original.isNull()) {
		qCWarning(logImageTools) << "leastBusyRegion: could not load" << imagePath;
		return QVariantMap {{"error", QStringLiteral("Image not found")}};
	}

	original = original.convertToFormat(QImage::Format_RGB32);
	auto working = scaleAndCropToScreen(original, screenWidth, screenHeight);
	auto gray = working.convertToFormat(QImage::Format_Grayscale8);

	auto w = gray.width();
	auto h = gray.height();
	if (w <= 0 || h <= 0) return QVariantMap {{"error", QStringLiteral("Image too small")}};

	// Clamp padding/region size to fit, exactly like least_busy_region.py.
	if (horizontalPadding * 2 >= w || verticalPadding * 2 >= h) {
		horizontalPadding = std::max(0, std::min(horizontalPadding, (w - 1) / 2));
		verticalPadding = std::max(0, std::min(verticalPadding, (h - 1) / 2));
	}

	auto maxRegionW = w - 2 * horizontalPadding;
	auto maxRegionH = h - 2 * verticalPadding;
	if (maxRegionW <= 0 || maxRegionH <= 0) {
		return QVariantMap {{"error", QStringLiteral("Image too small for the specified padding")}};
	}

	width = std::min(width, maxRegionW);
	height = std::min(height, maxRegionH);

	const IntegralImage integral(gray);

	const int stride = 10; // least_busy_region.py's CLI default; AbstractBackgroundWidget.qml
	                        // never overrides it.
	double area = static_cast<double>(width) * height;

	std::optional<double> bestVar;
	QPoint bestCoords(horizontalPadding, verticalPadding);

	int xStart = horizontalPadding;
	int yStart = verticalPadding;
	int xEnd = std::max(xStart, w - width - horizontalPadding);
	int yEnd = std::max(yStart, h - height - verticalPadding);

	for (int y = yStart; y <= yEnd; y += stride) {
		for (int x = xStart; x <= xEnd; x += stride) {
			int x2 = x + width - 1;
			int y2 = y + height - 1;
			if (x2 >= w || y2 >= h) continue;

			auto s = integral.regionSum(integral.sum, x, y, x2, y2);
			auto sSq = integral.regionSum(integral.sumSq, x, y, x2, y2);
			auto mean = s / area;
			auto var = (sSq / area) - (mean * mean);

			if (!bestVar.has_value() || (busiest ? var > *bestVar : var < *bestVar)) {
				bestVar = var;
				bestCoords = QPoint(x, y);
			}
		}
	}

	auto centerX = bestCoords.x() + width / 2;
	auto centerY = bestCoords.y() + height / 2;
	auto dominant =
	    dominantColorOfRegion(working, QRect(bestCoords.x(), bestCoords.y(), width, height));

	QVariantMap result;
	result["center_x"] = centerX;
	result["center_y"] = centerY;
	result["width"] = width;
	result["height"] = height;
	result["variance"] = bestVar.value_or(0.0);
	result["dominant_color"] = dominant;
	return result;
}

QVariantMap ImageTools::textColorFromImage(const QString& imagePath) {
	QImage img(imagePath);
	if (img.isNull()) {
		qCWarning(logImageTools) << "textColorFromImage: could not load" << imagePath;
		return QVariantMap {{"error", QStringLiteral("Could not decode image data")}};
	}

	img = img.convertToFormat(QImage::Format_RGB32);
	auto w = img.width();
	auto h = img.height();
	if (w <= 0 || h <= 0) return QVariantMap {{"error", QStringLiteral("Empty image")}};

	// 1-2. Corner pixels, median as the background anchor (handles noise/gradients better
	// than a plain average), exactly like text_color.py.
	std::array<QColor, 4> corners = {
	    img.pixelColor(0, 0),
	    img.pixelColor(w - 1, 0),
	    img.pixelColor(0, h - 1),
	    img.pixelColor(w - 1, h - 1),
	};
	std::array<int, 4> rs, gs, bs;
	for (int i = 0; i < 4; i++) {
		rs[i] = corners[i].red();
		gs[i] = corners[i].green();
		bs[i] = corners[i].blue();
	}
	std::sort(rs.begin(), rs.end());
	std::sort(gs.begin(), gs.end());
	std::sort(bs.begin(), bs.end());
	// Median of 4 = mean of the middle two (np.median behavior).
	auto bgR = (rs[1] + rs[2]) / 2.0;
	auto bgG = (gs[1] + gs[2]) / 2.0;
	auto bgB = (bs[1] + bs[2]) / 2.0;

	// 3. Distance of every pixel from the background; take the 95th percentile as the
	// threshold and median the pixels beyond it.
	std::vector<double> distances;
	distances.reserve(static_cast<size_t>(w) * h);
	std::vector<QRgb> pixels;
	pixels.reserve(static_cast<size_t>(w) * h);

	for (int y = 0; y < h; y++) {
		const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
		for (int x = 0; x < w; x++) {
			auto px = line[x];
			double dr = qRed(px) - bgR, dg = qGreen(px) - bgG, db = qBlue(px) - bgB;
			distances.push_back(std::sqrt(dr * dr + dg * dg + db * db));
			pixels.push_back(px);
		}
	}

	std::vector<double> sortedDistances = distances;
	std::sort(sortedDistances.begin(), sortedDistances.end());
	// numpy's default (linear-interpolation) percentile.
	double rank = 0.95 * (static_cast<double>(sortedDistances.size()) - 1);
	auto lo = static_cast<size_t>(std::floor(rank));
	auto hi = static_cast<size_t>(std::ceil(rank));
	double threshold = sortedDistances[lo]
	                  + (sortedDistances[hi] - sortedDistances[lo]) * (rank - static_cast<double>(lo));

	std::vector<int> tr, tg, tb;
	for (size_t i = 0; i < pixels.size(); i++) {
		if (distances[i] >= threshold) {
			tr.push_back(qRed(pixels[i]));
			tg.push_back(qGreen(pixels[i]));
			tb.push_back(qBlue(pixels[i]));
		}
	}

	int textR = 255, textG = 255, textB = 255; // fallback, matching text_color.py
	if (!tr.empty()) {
		auto median = [](std::vector<int> v) {
			std::sort(v.begin(), v.end());
			auto n = v.size();
			return n % 2 == 0 ? (v[n / 2 - 1] + v[n / 2]) / 2 : v[n / 2];
		};
		textR = median(tr);
		textG = median(tg);
		textB = median(tb);
	}

	QVariantMap result;
	result["background"] = toHex(static_cast<int>(bgR), static_cast<int>(bgG), static_cast<int>(bgB));
	result["text"] = toHex(textR, textG, textB);
	return result;
}

QString ImageTools::schemeForImage(const QString& imagePath) {
	QImage img(imagePath);
	if (img.isNull()) {
		qCWarning(logImageTools) << "schemeForImage: could not load" << imagePath;
		return QStringLiteral("scheme-tonal-spot");
	}

	img = img.convertToFormat(QImage::Format_RGB32);

	constexpr int maxDim = 128;
	if (std::max(img.width(), img.height()) > maxDim) {
		auto scale = static_cast<double>(maxDim) / std::max(img.width(), img.height());
		auto newW = std::max(1, static_cast<int>(img.width() * scale));
		auto newH = std::max(1, static_cast<int>(img.height() * scale));
		img = img.scaled(newW, newH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	}

	auto w = img.width();
	auto h = img.height();
	if (w <= 0 || h <= 0) return QStringLiteral("scheme-tonal-spot");

	// Hasler/Süsstrunk colorfulness metric, ported from scheme_for_image.py.
	double sumRg = 0, sumYb = 0, sumRgSq = 0, sumYbSq = 0;
	auto n = static_cast<double>(w) * h;

	for (int y = 0; y < h; y++) {
		const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
		for (int x = 0; x < w; x++) {
			auto px = line[x];
			double r = qRed(px), g = qGreen(px), b = qBlue(px);
			double rg = std::abs(r - g);
			double yb = std::abs(0.5 * (r + g) - b);
			sumRg += rg;
			sumYb += yb;
			sumRgSq += rg * rg;
			sumYbSq += yb * yb;
		}
	}

	auto meanRg = sumRg / n;
	auto meanYb = sumYb / n;
	auto varRg = (sumRgSq / n) - (meanRg * meanRg);
	auto varYb = (sumYbSq / n) - (meanYb * meanYb);
	auto stdRg = std::sqrt(std::max(0.0, varRg));
	auto stdYb = std::sqrt(std::max(0.0, varYb));

	auto colorfulness = std::sqrt(stdRg * stdRg + stdYb * stdYb)
	                   + (0.3 * std::sqrt(meanRg * meanRg + meanYb * meanYb));

	return colorfulness < 40 ? QStringLiteral("scheme-neutral") : QStringLiteral("scheme-tonal-spot");
}

QSize ImageTools::imageSize(const QString& imagePath) {
	QImageReader reader(imagePath);
	reader.setAutoTransform(true);
	auto size = reader.size();
	if (!size.isValid()) return {};

	// size() is the stored size; a rotated EXIF orientation swaps the sides on display.
	if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90)) size.transpose();
	return size;
}

} // namespace qs::windows::image
