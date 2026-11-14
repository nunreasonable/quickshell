#include "window_decorations.hpp"
#include <array>
#include <atomic>
#include <cstddef>

#include <qcolor.h>
#include <qcoreapplication.h>
#include <qhash.h>
#include <qobject.h>
#include <qpointer.h>
#include <qset.h>
#include <qstring.h>

#include "util.hpp"
#include "window_tracker.hpp"

#include <dwmapi.h>

namespace qs::windows {

namespace {

constexpr DWORD WINDOWS_11_BUILD = 22000;
constexpr std::size_t CRASH_SLOTS = 512;

std::array<std::atomic<HWND>, CRASH_SLOTS> gSlots {}; // NOLINT
std::atomic<bool> gBordersOverridden = false;          // NOLINT
std::atomic<bool> gCornersOverridden = false;          // NOLINT
std::atomic<bool> gOpacityOverridden = false;          // NOLINT

void rememberForCrash(HWND hwnd) {
	std::atomic<HWND>* free = nullptr;

	for (auto& slot: gSlots) {
		auto* value = slot.load();
		if (value == hwnd) return;
		if (value == nullptr && free == nullptr) free = &slot;
	}

	if (free != nullptr) free->store(hwnd);
}

void forgetForCrash(HWND hwnd) {
	for (auto& slot: gSlots) {
		if (slot.load() == hwnd) slot.store(nullptr);
	}
}

COLORREF colorRef(const QColor& color) {
	auto rgb = color.toRgb();
	return RGB(rgb.red(), rgb.green(), rgb.blue());
}

bool setBorder(HWND hwnd, COLORREF color) {
	return SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color)));
}

bool setCorner(HWND hwnd, DWORD preference) {
	auto value = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(preference);
	return SUCCEEDED(
	    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &value, sizeof(value))
	);
}

bool reachable(HWND hwnd) { return IsWindow(hwnd) && !IsHungAppWindow(hwnd); }

bool coversMonitor(HWND hwnd) {
	RECT rect {};
	if (!GetWindowRect(hwnd, &rect)) return false;

	MONITORINFO info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return false;

	const auto& screen = info.rcMonitor;
	return rect.left <= screen.left && rect.top <= screen.top && rect.right >= screen.right
	    && rect.bottom >= screen.bottom;
}

bool layered(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0; }

} // namespace

DecorationManager* DecorationManager::instance() {
	static QPointer<DecorationManager> manager; // NOLINT

	if (manager.isNull()) {
		manager = new DecorationManager(QCoreApplication::instance());
	}

	return manager.data();
}

DecorationManager::DecorationManager(QObject* parent): QObject(parent) {
	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
		this->restoreAll();
		this->stop();
	});
}

DecorationManager::~DecorationManager() { this->restoreAll(); }

bool DecorationManager::supported() { return windowsBuild() >= WINDOWS_11_BUILD; }

void DecorationManager::restoreForCrash() {
	auto borders = gBordersOverridden.load();
	auto corners = gCornersOverridden.load();
	auto opacity = gOpacityOverridden.load();
	if (!borders && !corners && !opacity) return;

	for (auto& slot: gSlots) {
		auto* hwnd = slot.load();
		if (hwnd == nullptr || !reachable(hwnd)) continue;
		if (borders) setBorder(hwnd, DWMWA_COLOR_DEFAULT);
		if (corners) setCorner(hwnd, DWMWCP_DEFAULT);
		if (opacity && layered(hwnd)) SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
	}
}

void DecorationManager::setBorders(const QString& borders) {
	auto value = borders.trimmed().toLower();
	if (value != QStringLiteral("colors") && value != QStringLiteral("none")) {
		value = QStringLiteral("system");
	}

	if (value == this->mBorders) return;
	this->mBorders = value;
	this->update();
	emit this->bordersChanged();
}

void DecorationManager::setActiveColor(const QColor& color) {
	if (color == this->mActiveColor) return;
	this->mActiveColor = color;
	if (this->borderMode() == BorderMode::Colors) this->update();
	emit this->activeColorChanged();
}

void DecorationManager::setInactiveColor(const QColor& color) {
	if (color == this->mInactiveColor) return;
	this->mInactiveColor = color;
	if (this->borderMode() == BorderMode::Colors) this->update();
	emit this->inactiveColorChanged();
}

void DecorationManager::setCorners(const QString& corners) {
	auto value = corners.trimmed();
	if (value != QStringLiteral("round") && value != QStringLiteral("roundSmall")
	    && value != QStringLiteral("square"))
	{
		value = QStringLiteral("default");
	}

	if (value == this->mCorners) return;
	this->mCorners = value;
	this->update();
	emit this->cornersChanged();
}

void DecorationManager::setActiveOpacity(qreal opacity) {
	opacity = qBound(0.1, opacity, 1.0);
	if (qFuzzyCompare(opacity, this->mActiveOpacity)) return;
	this->mActiveOpacity = opacity;
	emit this->activeOpacityChanged();
	this->update();
}

void DecorationManager::setInactiveOpacity(qreal opacity) {
	opacity = qBound(0.1, opacity, 1.0);
	if (qFuzzyCompare(opacity, this->mInactiveOpacity)) return;
	this->mInactiveOpacity = opacity;
	emit this->inactiveOpacityChanged();
	this->update();
}

DecorationManager::BorderMode DecorationManager::borderMode() const {
	if (this->mBorders == QStringLiteral("colors")) return BorderMode::Colors;
	if (this->mBorders == QStringLiteral("none")) return BorderMode::None;
	return BorderMode::System;
}

DWORD DecorationManager::cornerPreference() const {
	if (this->mCorners == QStringLiteral("round")) return DWMWCP_ROUND;
	if (this->mCorners == QStringLiteral("roundSmall")) return DWMWCP_ROUNDSMALL;
	if (this->mCorners == QStringLiteral("square")) return DWMWCP_DONOTROUND;
	return DWMWCP_DEFAULT;
}

bool DecorationManager::overriding() const {
	if (this->opacityOverriding()) return true;
	if (!DecorationManager::supported()) return false;
	return this->borderMode() != BorderMode::System || this->cornerPreference() != DWMWCP_DEFAULT;
}

bool DecorationManager::opacityOverriding() const {
	return this->alphaFor(true) != 255 || this->alphaFor(false) != 255;
}

BYTE DecorationManager::alphaFor(bool active) const {
	auto opacity = active ? this->mActiveOpacity : this->mInactiveOpacity;
	return static_cast<BYTE>(qRound(qBound(0.1, opacity, 1.0) * 255));
}

void DecorationManager::update() {
	auto supported = DecorationManager::supported();
	auto borders = supported && this->borderMode() != BorderMode::System;
	auto corners = supported && this->cornerPreference() != DWMWCP_DEFAULT;

	auto opacity = this->opacityOverriding();

	if (borders) gBordersOverridden.store(true);
	if (corners) gCornersOverridden.store(true);
	if (opacity) gOpacityOverridden.store(true);

	if (this->overriding()) {
		if (!this->running) this->start();
		this->applyAll();
	} else if (this->running) {
		this->applyAll();
		this->stop();
	}

	gBordersOverridden.store(borders);
	gCornersOverridden.store(corners);
	gOpacityOverridden.store(opacity);
}

void DecorationManager::start() {
	this->tracker = WindowTracker::instance();
	this->running = true;

	auto& c = this->connections;
	c.append(QObject::connect(this->tracker, &WindowTracker::windowAdded, this, [this](TrackedWindow* window) {
		this->watch(window);
		this->apply(window);
	}));
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowRemoved, this, &DecorationManager::forget)
	);
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::activeWindowChanged,
	    this,
	    &DecorationManager::onActiveWindowChanged
	));

	auto* active = this->tracker->activeWindow();
	this->lastActive = active == nullptr ? nullptr : active->hwnd();
}

void DecorationManager::stop() {
	for (const auto& connection: this->connections) QObject::disconnect(connection);
	this->connections.clear();
	this->watched.clear();
	this->restoreAll();
	this->lastActive = nullptr;
	this->running = false;
}

void DecorationManager::applyAll() {
	if (this->tracker == nullptr) return;

	QSet<HWND> tracked;
	for (auto* window: this->tracker->windows()) {
		tracked.insert(window->hwnd());
		if (this->running) this->watch(window);
		this->apply(window);
	}

	for (auto iter = this->applied.begin(); iter != this->applied.end();) {
		if (tracked.contains(iter.key())) {
			++iter;
			continue;
		}

		DecorationManager::restore(iter.key(), iter.value());
		forgetForCrash(iter.key());
		iter = this->applied.erase(iter);
	}
}

void DecorationManager::apply(TrackedWindow* window) {
	if (window == nullptr || this->tracker == nullptr) return;

	auto* hwnd = window->hwnd();
	if (isOwnProcessWindow(hwnd) || !reachable(hwnd)) return;

	auto state = this->applied.value(hwnd);

	auto supported = DecorationManager::supported();
	auto active = window == this->tracker->activeWindow();
	auto mode = supported ? this->borderMode() : BorderMode::System;
	auto color = active ? this->mActiveColor : this->mInactiveColor;
	if (mode == BorderMode::Colors && !color.isValid()) mode = BorderMode::System;

	if (mode != BorderMode::System) {
		auto value = mode == BorderMode::None ? DWMWA_COLOR_NONE : colorRef(color);
		if ((!state.border || state.color != value) && setBorder(hwnd, value)) {
			state.border = true;
			state.color = value;
		}
	} else if (state.border) {
		setBorder(hwnd, DWMWA_COLOR_DEFAULT);
		state.border = false;
	}

	auto preference = supported ? this->cornerPreference() : DWORD(DWMWCP_DEFAULT);
	if (preference != DWMWCP_DEFAULT) {
		if ((!state.corner || state.preference != preference) && setCorner(hwnd, preference)) {
			state.corner = true;
			state.preference = preference;
		}
	} else if (state.corner) {
		setCorner(hwnd, DWMWCP_DEFAULT);
		state.corner = false;
	}

	auto alpha = this->alphaFor(active);
	if (alpha != 255 && !coversMonitor(hwnd)) {
		if (!state.addedLayered && !layered(hwnd)) {
			SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
			state.addedLayered = layered(hwnd);
		}
		if (state.addedLayered && (!state.opacity || state.alpha != alpha)
		    && SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA))
		{
			state.opacity = true;
			state.alpha = alpha;
		}
	} else if (state.opacity || state.addedLayered) {
		DecorationManager::restoreOpacity(hwnd, state);
	}

	if (state.border || state.corner || state.opacity || state.addedLayered) {
		this->applied.insert(hwnd, state);
		rememberForCrash(hwnd);
	} else {
		this->applied.remove(hwnd);
		forgetForCrash(hwnd);
	}
}

void DecorationManager::forget(TrackedWindow* window) {
	if (window == nullptr) return;

	auto* hwnd = window->hwnd();
	if (hwnd == this->lastActive) this->lastActive = nullptr;
	this->watched.remove(window);

	auto iter = this->applied.find(hwnd);
	if (iter == this->applied.end()) return;

	DecorationManager::restore(hwnd, iter.value());

	forgetForCrash(hwnd);
	this->applied.erase(iter);
}

void DecorationManager::restoreAll() {
	for (auto iter = this->applied.cbegin(); iter != this->applied.cend(); ++iter) {
		DecorationManager::restore(iter.key(), iter.value());
		forgetForCrash(iter.key());
	}

	this->applied.clear();
}

void DecorationManager::watch(TrackedWindow* window) {
	if (window == nullptr || this->watched.contains(window)) return;
	this->watched.insert(window);

	auto reapply = [this, window]() {
		if (this->running && this->opacityOverriding()) this->apply(window);
	};
	this->connections.append(QObject::connect(window, &TrackedWindow::rectChanged, this, reapply));
	this->connections.append(QObject::connect(window, &TrackedWindow::fullscreenChanged, this, reapply));
}

void DecorationManager::restore(HWND hwnd, const Applied& state) {
	if (!reachable(hwnd)) return;
	if (state.border) setBorder(hwnd, DWMWA_COLOR_DEFAULT);
	if (state.corner) setCorner(hwnd, DWMWCP_DEFAULT);

	auto copy = state;
	DecorationManager::restoreOpacity(hwnd, copy);
}

void DecorationManager::restoreOpacity(HWND hwnd, Applied& state) {
	if (reachable(hwnd) && state.addedLayered) {
		SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
		RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
	}

	state.opacity = false;
	state.addedLayered = false;
	state.alpha = 255;
}

void DecorationManager::onActiveWindowChanged() {
	auto* active = this->tracker->activeWindow();
	auto* hwnd = active == nullptr ? nullptr : active->hwnd();
	if (hwnd == this->lastActive) return;

	if (auto* previous = this->tracker->windowFor(this->lastActive)) this->apply(previous);
	this->lastActive = hwnd;
	if (active != nullptr) this->apply(active);
}

} // namespace qs::windows
