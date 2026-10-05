#include "blur_shapes.hpp"
#include <algorithm>
#include <cmath>

#include <qcolor.h>
#include <qjsvalue.h>
#include <qlist.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qrect.h>
#include <qregion.h>
#include <qtypes.h>
#include <private/qquickrectangle_p.h>

namespace qs::windows {

namespace {

constexpr qreal OPAQUE_ALPHA = 0.995;
constexpr qreal INVISIBLE_ALPHA = 0.001;
constexpr qreal EDGE_INSET = 1.0;
constexpr int ITEM_BUDGET = 4000;
constexpr int MAX_DEPTH = 96;
constexpr qsizetype MAX_SHAPES = 48;

qreal fillAlpha(const QQuickRectangle* rectangle) {
	auto gradient = rectangle->gradient();

	if (gradient.isQObject()) {
		if (auto* object = qobject_cast<QQuickGradient*>(gradient.toQObject())) {
			qreal alpha = 0;
			for (const auto& stop: object->gradientStops()) {
				alpha = std::max(alpha, static_cast<qreal>(stop.second.alphaF()));
			}
			return alpha;
		}
	} else if (gradient.isNumber() || gradient.isString()) {
		return 1;
	}

	return rectangle->color().alphaF();
}

qreal cornerRadius(const QQuickRectangle* rectangle) {
	return std::max(
	    {rectangle->topLeftRadius(),
	     rectangle->topRightRadius(),
	     rectangle->bottomLeftRadius(),
	     rectangle->bottomRightRadius(),
	     0.0}
	);
}

class Collector {
public:
	explicit Collector(const BlurShapeQuery& query): query(query) {}

	BlurShapeResult run(QQuickWindow* window) {
		if (window == nullptr || window->contentItem() == nullptr) return {};
		if (this->query.hasMask && this->query.mask.isEmpty()) return {};

		auto windowRect = QRectF(0, 0, window->width(), window->height());
		auto base = static_cast<qreal>(window->color().alphaF());
		if (base >= OPAQUE_ALPHA) return {};

		auto wholeSurface = [&]() {
			if (this->query.hasMask) {
				for (const auto& rect: this->query.mask) this->add(rect, 0, windowRect);
			} else {
				this->add(windowRect, 0, windowRect);
			}
		};

		if (!this->query.ignoreAlpha.has_value()) {
			wholeSurface();
		} else if (base > *this->query.ignoreAlpha) {
			wholeSurface();
		} else {
			this->threshold = *this->query.ignoreAlpha;
			this->walk(window->contentItem(), 1, base, windowRect, 0);
		}

		return {.shapes = this->shapes, .truncated = this->budget < 0};
	}

private:
	void walk(QQuickItem* item, qreal opacity, qreal coverage, const QRectF& clip, int depth) {
		if (item == nullptr || depth > MAX_DEPTH || --this->budget < 0) return;
		if (!item->isVisible()) return;

		opacity *= item->opacity();
		if (opacity <= INVISIBLE_ALPHA) return;

		auto* rectangle = qobject_cast<QQuickRectangle*>(item);
		auto clips = item->clip();

		QRectF sceneRect;
		if (rectangle != nullptr || clips) {
			sceneRect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
		}

		auto childClip = clip;
		if (clips) {
			childClip = clip.intersected(sceneRect);
			if (childClip.isEmpty()) return;
		}

		if (rectangle != nullptr) {
			auto alpha = std::clamp(fillAlpha(rectangle) * opacity, 0.0, 1.0);
			auto combined = 1.0 - (1.0 - coverage) * (1.0 - alpha);

			if (combined >= OPAQUE_ALPHA) return;

			if (combined > this->threshold) {
				auto scale = item->width() > 0 ? sceneRect.width() / item->width() : 1.0;
				this->add(sceneRect, cornerRadius(rectangle) * scale, childClip);
				return;
			}

			coverage = combined;
		}

		const auto children = item->childItems();
		for (auto* child: children) {
			this->walk(child, opacity, coverage, childClip, depth + 1);
		}
	}

	void add(const QRectF& rect, qreal radius, const QRectF& clip) {
		if (this->shapes.size() >= MAX_SHAPES) return;

		auto visible = rect.intersected(clip);
		if (visible.isEmpty()) return;
		if (this->query.hasMask && !this->query.mask.intersects(visible.toAlignedRect())) return;

		auto dpr = this->query.dpr;
		auto toPhysical = [dpr](const QRectF& r) {
			return QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr);
		};

		auto physical = toPhysical(rect).adjusted(EDGE_INSET, EDGE_INSET, -EDGE_INSET, -EDGE_INSET);
		if (physical.width() < 1 || physical.height() < 1) return;

		auto maxRadius = std::min(physical.width(), physical.height()) / 2;
		auto physicalRadius = std::clamp(radius * dpr - EDGE_INSET, 0.0, maxRadius);

		this->shapes.append(BlurShape {
		    .rect = physical,
		    .radius = physicalRadius,
		    .clip = toPhysical(clip),
		});
	}

	const BlurShapeQuery& query;
	QList<BlurShape> shapes;
	qreal threshold = 0;
	int budget = ITEM_BUDGET;
};

} // namespace

bool BlurShape::fuzzyEquals(const BlurShape& other) const {
	auto same = [](qreal a, qreal b) { return std::abs(a - b) < 0.01; };
	auto sameRect = [&](const QRectF& a, const QRectF& b) {
		return same(a.x(), b.x()) && same(a.y(), b.y()) && same(a.width(), b.width())
		    && same(a.height(), b.height());
	};

	return sameRect(this->rect, other.rect) && same(this->radius, other.radius)
	    && sameRect(this->clip, other.clip);
}

BlurShapeResult collectBlurShapes(QQuickWindow* window, const BlurShapeQuery& query) {
	return Collector(query).run(window);
}

} // namespace qs::windows
