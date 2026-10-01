#include "panel_window.hpp"
#include <algorithm>

#include <qt_windows.h>

#include <qbytearray.h>
#include <qevent.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qqmlengine.h>
#include <qquickwindow.h>
#include <qrect.h>
#include <qregion.h>
#include <qscreen.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <shellapi.h>

#include "../core/qmlscreen.hpp"
#include "../core/types.hpp"
#include "../window/panelinterface.hpp"
#include "../window/proxywindow.hpp"
#include "appbar.hpp"
#include "input_mask.hpp"
#include "util.hpp"

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logPanel, "quickshell.windows.panel", QtWarningMsg);
}

// Every visible panel, for operations that span windows: keeping Overlay panels above Top
// panels and lowering Top panels while a fullscreen application is active.
class WinPanelStack {
public:
	static WinPanelStack* instance() {
		static WinPanelStack* stack = nullptr; // NOLINT

		if (stack == nullptr) {
			stack = new WinPanelStack();
		}

		return stack;
	}

	void addPanel(WinPanelWindow* panel) {
		if (!this->mPanels.contains(panel)) this->mPanels.push_back(panel);
	}

	void removePanel(WinPanelWindow* panel) { this->mPanels.removeOne(panel); }

	// Topmost windows are ordered by when they were last raised, so overlays have to be
	// re-raised whenever another panel was shown or restacked.
	void raiseOverlays() {
		for (auto* panel: this->mPanels) {
			if (panel->bLayer != PanelLayer::Overlay) continue;
			auto* hwnd = panel->hwnd();
			if (hwnd == nullptr) continue;
			SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
	}

	[[nodiscard]] bool fullscreenAppActive() const { return this->mFullscreenApp; }

	void setFullscreenAppActive(bool active) {
		if (active == this->mFullscreenApp) return;
		this->mFullscreenApp = active;

		for (auto* panel: this->mPanels) {
			if (panel->bLayer == PanelLayer::Top) panel->updateLayer();
		}
	}

private:
	QList<WinPanelWindow*> mPanels;
	bool mFullscreenApp = false;
};

// WinProxiedWindow

bool WinProxiedWindow::event(QEvent* event) {
	if (event->type() == QEvent::PlatformSurface) {
		auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event); // NOLINT

		if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
			emit this->surfaceCreated();
		}
	}

	return this->ProxiedWindow::event(event);
}

bool WinProxiedWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
	if (eventType == "windows_generic_MSG") {
		if (auto* panel = qobject_cast<WinPanelWindow*>(this->proxy())) {
			if (panel->handleNativeMessage(static_cast<MSG*>(message), result)) return true;
		}
	}

	return this->ProxiedWindow::nativeEvent(eventType, message, result);
}

// WinPanelWindow

WinPanelWindow::WinPanelWindow(QObject* parent): ProxyWindowBase(parent) {
	this->bcExclusiveZone.setBinding([this]() -> qint32 {
		switch (this->bExclusionMode.value()) {
		case ExclusionMode::Ignore: return 0;
		case ExclusionMode::Normal: return this->bExclusiveZone;
		case ExclusionMode::Auto:
			auto edge = this->bcExclusionEdge.value();
			auto margins = this->bMargins.value();

			if (edge == Qt::TopEdge || edge == Qt::BottomEdge) {
				return this->bImplicitHeight + margins.top + margins.bottom;
			} else if (edge == Qt::LeftEdge || edge == Qt::RightEdge) {
				return this->bImplicitWidth + margins.left + margins.right;
			} else {
				return 0;
			}
		}

		return 0;
	});

	this->bcExclusionEdge.setBinding([this] { return this->bAnchors.value().exclusionEdge(); });
}

WinPanelWindow::~WinPanelWindow() {
	// The base destructor only runs the base disownWindow, so release the native state here
	// while the backing window still exists.
	this->releaseNativeState();
}

ProxiedWindow* WinPanelWindow::retrieveWindow(QObject* oldInstance) {
	auto* old = qobject_cast<WinPanelWindow*>(oldInstance);
	if (old == nullptr) return nullptr;

	// Take over the AppBar registration instead of removing and re-adding it, which would
	// make every maximized window re-layout on each reload.
	this->appBar.adopt(old->appBar);

	return old->disownWindow();
}

ProxiedWindow* WinPanelWindow::createQQuickWindow() { return new WinProxiedWindow(this); }

void WinPanelWindow::connectWindow() {
	this->ProxyWindowBase::connectWindow();

	if (auto* window = qobject_cast<WinProxiedWindow*>(this->window)) {
		QObject::connect(
		    window,
		    &WinProxiedWindow::surfaceCreated,
		    this,
		    &WinPanelWindow::onSurfaceCreated
		);
	} else {
		qCWarning(logPanel) << "Backing window of" << this
		                    << "is not a WinProxiedWindow, native messages will be lost.";
	}

	// clang-format off
	QObject::connect(this->window, &QQuickWindow::visibleChanged, this, &WinPanelWindow::onWindowVisibleChanged);
	QObject::connect(this->window, &QWindow::screenChanged, this, &WinPanelWindow::updateScreen);
	QObject::connect(this->window, &ProxiedWindow::devicePixelRatioChanged, this, &WinPanelWindow::scheduleUpdateDimensions);
	// clang-format on

	this->updateScreen();

	// Qt::Tool gives WS_EX_TOOLWINDOW: no taskbar button, no alt-tab entry, and the shell shows
	// such windows on every virtual desktop, which is what a panel wants. (Real pinning through
	// the virtual desktop interfaces is a later task.) Frameless: WS_POPUP without a caption.
	this->window->setFlags(Qt::Tool | Qt::FramelessWindowHint);
	this->updateLayer();
	this->updateFocus();

	if (this->window->handle() != nullptr) {
		this->nativeInit();
	} else {
		// geometry before creation so the HWND is created in place; the AppBar needs the HWND
		this->updateDimensions();
	}
}

ProxiedWindow* WinPanelWindow::disownWindow(bool keepItemOwnership) {
	this->releaseNativeState();
	return this->ProxyWindowBase::disownWindow(keepItemOwnership);
}

void WinPanelWindow::releaseNativeState() {
	if (this->window != nullptr) {
		InputMaskTracker::instance()->remove(this->window);
	}

	this->appBar.remove();
	WinPanelStack::instance()->removePanel(this);
}

void WinPanelWindow::trySetWidth(qint32 implicitWidth) {
	// only update the actual size if not blocked by anchors
	if (!this->bAnchors.value().horizontalConstraint()) {
		this->ProxyWindowBase::trySetWidth(implicitWidth);
		this->updateDimensions();
	}
}

void WinPanelWindow::trySetHeight(qint32 implicitHeight) {
	// only update the actual size if not blocked by anchors
	if (!this->bAnchors.value().verticalConstraint()) {
		this->ProxyWindowBase::trySetHeight(implicitHeight);
		this->updateDimensions();
	}
}

void WinPanelWindow::setScreen(QuickshellScreenInfo* screen) {
	this->ProxyWindowBase::setScreen(screen);
	this->updateScreen();
}

bool WinPanelWindow::aboveWindows() const { return this->bLayer.value() > PanelLayer::Bottom; }

void WinPanelWindow::setAboveWindows(bool aboveWindows) {
	this->setLayer(aboveWindows ? PanelLayer::Top : PanelLayer::Bottom);
}

bool WinPanelWindow::focusable() const {
	return this->bKeyboardFocus.value() != PanelKeyboardFocus::None;
}

void WinPanelWindow::setFocusable(bool focusable) {
	this->setKeyboardFocus(focusable ? PanelKeyboardFocus::OnDemand : PanelKeyboardFocus::None);
}

HWND WinPanelWindow::hwnd() const { return hwndOf(this->window); }

void WinPanelWindow::onSurfaceCreated() { this->nativeInit(); }

void WinPanelWindow::nativeInit() {
	if (this->hwnd() == nullptr) return;

	applyPanelDwmAttributes(this->hwnd());
	this->applyNativeStyles();
	this->updateDimensions();
	this->updateLayer();
	this->updateFocus();
}

void WinPanelWindow::applyNativeStyles() {
	// Qt::Tool already requests WS_EX_TOOLWINDOW; enforce it in case the flag mapping changes.
	setExStyleBits(this->hwnd(), WS_EX_TOOLWINDOW, true);
}

void WinPanelWindow::onWindowVisibleChanged() {
	if (this->window->isVisible()) {
		WinPanelStack::instance()->addPanel(this);
		this->updateDimensions();
		this->updateLayer();

		if (this->bKeyboardFocus == PanelKeyboardFocus::Exclusive) this->grabKeyboardFocus();
	} else {
		// Hidden AppBars still reserve space, so drop the reservation with the window.
		this->appBar.remove();
		WinPanelStack::instance()->removePanel(this);
	}

	InputMaskTracker::instance()->refresh();
}

void WinPanelWindow::updateScreen() {
	auto* newScreen =
	    this->mScreen ? this->mScreen : (this->window ? this->window->screen() : nullptr);

	if (newScreen == this->mTrackedScreen) return;

	if (this->mTrackedScreen != nullptr) {
		QObject::disconnect(this->mTrackedScreen, nullptr, this, nullptr);
	}

	this->mTrackedScreen = newScreen;

	if (this->mTrackedScreen != nullptr) {
		// clang-format off
		QObject::connect(this->mTrackedScreen, &QScreen::geometryChanged, this, &WinPanelWindow::scheduleUpdateDimensions);
		QObject::connect(this->mTrackedScreen, &QScreen::availableGeometryChanged, this, &WinPanelWindow::scheduleUpdateDimensions);
		QObject::connect(this->mTrackedScreen, &QScreen::logicalDotsPerInchChanged, this, &WinPanelWindow::scheduleUpdateDimensions);
		// clang-format on
	}

	this->updateDimensions();
}

void WinPanelWindow::scheduleUpdateDimensions() {
	// Work area and AppBar notifications arrive in bursts (one per AppBar on the system),
	// and each update talks to explorer synchronously, so coalesce them.
	if (this->dimensionsUpdatePending) return;
	this->dimensionsUpdatePending = true;

	QTimer::singleShot(0, this, [this]() {
		this->dimensionsUpdatePending = false;
		this->updateDimensions();
	});
}

void WinPanelWindow::updateDimensions() {
	if (this->window == nullptr || this->mTrackedScreen == nullptr) return;

	auto* screen = this->mTrackedScreen.data();
	auto screenGeometry = screen->geometry();
	auto rects = monitorRects(monitorForScreen(screen));
	auto mapper = ScreenMapper(screenGeometry, rects.monitor, screen->devicePixelRatio());
	auto* hwnd = this->hwnd();

	// Normal mode with a negative zone matches the layer-shell meaning of -1: ignore zones.
	auto ignoreZones = this->bExclusionMode == ExclusionMode::Ignore
	                || (this->bExclusionMode == ExclusionMode::Normal && this->bExclusiveZone < 0);

	// The work area already excludes the taskbar and every AppBar, including the ones
	// registered by other panels, which is what the X11 backend computes by hand.
	QRect base;
	if (ignoreZones) base = screenGeometry;
	else if (rects.valid) base = mapper.toLogical(rects.work);
	else base = screen->availableGeometry();

	auto edge = this->bcExclusionEdge.value();
	auto zone = this->bcExclusiveZone.value();
	auto wantsAppBar = !ignoreZones && edge != 0 && zone > 0 && hwnd != nullptr && rects.valid;

	if (wantsAppBar) {
		UINT abEdge = ABE_TOP;
		switch (edge) {
		case Qt::LeftEdge: abEdge = ABE_LEFT; break;
		case Qt::RightEdge: abEdge = ABE_RIGHT; break;
		case Qt::TopEdge: abEdge = ABE_TOP; break;
		case Qt::BottomEdge: abEdge = ABE_BOTTOM; break;
		default: break;
		}

		auto reserved = this->appBar.reserve(hwnd, abEdge, rects.monitor, mapper.toPhysical(zone));

		if (reserved.isValid()) {
			// The work area excludes our own reservation as well; the panel lives inside it.
			auto reservedLogical = mapper.toLogical(reserved);

			switch (edge) {
			case Qt::TopEdge: base.setTop(std::min(base.top(), reservedLogical.top())); break;
			case Qt::BottomEdge: base.setBottom(std::max(base.bottom(), reservedLogical.bottom())); break;
			case Qt::LeftEdge: base.setLeft(std::min(base.left(), reservedLogical.left())); break;
			case Qt::RightEdge: base.setRight(std::max(base.right(), reservedLogical.right())); break;
			default: break;
			}
		}
	} else {
		this->appBar.remove();
	}

	auto geometry = QRect();

	auto anchors = this->bAnchors.value();
	auto margins = this->bMargins.value();

	if (anchors.horizontalConstraint()) {
		geometry.setX(base.x() + margins.left);
		geometry.setWidth(base.width() - margins.left - margins.right);
	} else {
		if (anchors.mLeft) {
			geometry.setX(base.x() + margins.left);
		} else if (anchors.mRight) {
			geometry.setX(base.x() + base.width() - this->implicitWidth() - margins.right);
		} else {
			geometry.setX(base.x() + base.width() / 2 - this->implicitWidth() / 2);
		}

		geometry.setWidth(this->implicitWidth());
	}

	if (anchors.verticalConstraint()) {
		geometry.setY(base.y() + margins.top);
		geometry.setHeight(base.height() - margins.top - margins.bottom);
	} else {
		if (anchors.mTop) {
			geometry.setY(base.y() + margins.top);
		} else if (anchors.mBottom) {
			geometry.setY(base.y() + base.height() - this->implicitHeight() - margins.bottom);
		} else {
			geometry.setY(base.y() + base.height() / 2 - this->implicitHeight() / 2);
		}

		geometry.setHeight(this->implicitHeight());
	}

	if (this->window->geometry() != geometry) {
		qCDebug(logPanel) << "Placing" << this << "at" << geometry << "on" << screen->name();
		this->window->setGeometry(geometry);
	}
}

void WinPanelWindow::updateLayer() {
	if (this->window == nullptr) return;

	auto layer = this->bLayer.value();

	// Qt keeps WS_EX_TOPMOST in sync with this flag across its own style rewrites.
	this->window->setFlag(Qt::WindowStaysOnTopHint, layer >= PanelLayer::Top);

	auto* hwnd = this->hwnd();
	if (hwnd == nullptr) return;

	this->applyNativeStyles();

	HWND insertAfter = nullptr;
	switch (layer) {
	// Progman (the desktop) is kept bottom-most by the window manager, so HWND_BOTTOM lands
	// directly above it. WM_WINDOWPOSCHANGING keeps the window there.
	case PanelLayer::Background: insertAfter = HWND_BOTTOM; break;
	case PanelLayer::Bottom: insertAfter = HWND_NOTOPMOST; break;
	// Like the taskbar: drop out of the topmost band while a fullscreen app is active.
	case PanelLayer::Top:
		insertAfter = WinPanelStack::instance()->fullscreenAppActive() ? HWND_BOTTOM : HWND_TOPMOST;
		break;
	case PanelLayer::Overlay: insertAfter = HWND_TOPMOST; break;
	}

	SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	if (layer != PanelLayer::Overlay) WinPanelStack::instance()->raiseOverlays();

	// style rewrites by qt drop WS_EX_TRANSPARENT
	InputMaskTracker::instance()->refresh();
}

void WinPanelWindow::updateFocus() {
	if (this->window == nullptr) return;

	auto focus = this->bKeyboardFocus.value();

	// WS_EX_NOACTIVATE: the window receives mouse input but never becomes the active window,
	// so the application underneath keeps keyboard focus.
	this->window->setFlag(Qt::WindowDoesNotAcceptFocus, focus == PanelKeyboardFocus::None);

	if (focus == PanelKeyboardFocus::Exclusive && this->isVisibleDirect()) {
		this->grabKeyboardFocus();
	}

	InputMaskTracker::instance()->refresh();
}

void WinPanelWindow::grabKeyboardFocus() {
	auto* hwnd = this->hwnd();
	if (hwnd == nullptr) return;

	this->window->requestActivate();
	if (GetForegroundWindow() != hwnd && !forceForegroundWindow(hwnd)) {
		qCWarning(logPanel) << "Could not take exclusive keyboard focus for" << this;
	}
}

void WinPanelWindow::scheduleFocusGrab() {
	if (this->focusGrabPending) return;
	this->focusGrabPending = true;

	// Deferred and rate limited: WM_ACTIVATE arrives in the middle of the activation change,
	// and fighting another window on every message would hog both processes.
	QTimer::singleShot(50, this, [this]() {
		this->focusGrabPending = false;

		if (this->window == nullptr || !this->isVisibleDirect()
		    || this->bKeyboardFocus != PanelKeyboardFocus::Exclusive)
			return;

		auto* foreground = GetForegroundWindow();
		// Popups and menus of this process are allowed to take focus.
		if (foreground == this->hwnd() || isOwnProcessWindow(foreground)) return;

		this->grabKeyboardFocus();
	});
}

void WinPanelWindow::applyInputMask(const QRegion& region, bool hasMask) {
	if (this->window == nullptr) return;

	if (hasMask) InputMaskTracker::instance()->setMask(this->window, region);
	else InputMaskTracker::instance()->remove(this->window);
}

bool WinPanelWindow::handleNativeMessage(MSG* msg, qintptr* result) {
	if (this->window == nullptr) return false;

	if (msg->message == WinAppBar::callbackMessage()) {
		switch (msg->wParam) {
		case ABN_POSCHANGED: this->scheduleUpdateDimensions(); break;
		case ABN_FULLSCREENAPP:
			WinPanelStack::instance()->setFullscreenAppActive(msg->lParam != 0);
			break;
		default: break;
		}

		*result = 0;
		return true;
	}

	if (msg->message == WinAppBar::taskbarCreatedMessage()) {
		// Explorer restarted: all AppBar registrations and the work area are gone.
		this->appBar.invalidate();
		this->scheduleUpdateDimensions();
		return false;
	}

	switch (msg->message) {
	case WM_ACTIVATE:
		this->appBar.notifyActivate();

		if (LOWORD(msg->wParam) == WA_INACTIVE
		    && this->bKeyboardFocus == PanelKeyboardFocus::Exclusive)
		{
			this->scheduleFocusGrab();
		}
		break;
	case WM_WINDOWPOSCHANGED:
		this->appBar.notifyWindowPosChanged();
		InputMaskTracker::instance()->refresh();
		break;
	case WM_WINDOWPOSCHANGING:
		if (this->bLayer == PanelLayer::Background) {
			auto* pos = reinterpret_cast<WINDOWPOS*>(msg->lParam); // NOLINT(performance-no-int-to-ptr)
			// Activation and qt's style rewrites try to raise the window; keep it on the desktop.
			if (!(pos->flags & SWP_NOZORDER)) pos->hwndInsertAfter = HWND_BOTTOM;
		}
		break;
	default: break;
	}

	return false;
}

// WinPanelInterface

WinPanelInterface::WinPanelInterface(QObject* parent)
    : PanelWindowInterface(parent)
    , panel(new WinPanelWindow(this)) {
	this->connectSignals();

	// clang-format off
	QObject::connect(this->panel, &WinPanelWindow::anchorsChanged, this, &WinPanelInterface::anchorsChanged);
	QObject::connect(this->panel, &WinPanelWindow::marginsChanged, this, &WinPanelInterface::marginsChanged);
	QObject::connect(this->panel, &WinPanelWindow::exclusiveZoneChanged, this, &WinPanelInterface::exclusiveZoneChanged);
	QObject::connect(this->panel, &WinPanelWindow::exclusionModeChanged, this, &WinPanelInterface::exclusionModeChanged);
	QObject::connect(this->panel, &WinPanelWindow::layerChanged, this, &WinPanelInterface::aboveWindowsChanged);
	QObject::connect(this->panel, &WinPanelWindow::keyboardFocusChanged, this, &WinPanelInterface::focusableChanged);
	// clang-format on
}

void WinPanelInterface::onReload(QObject* oldInstance) {
	QQmlEngine::setContextForObject(this->panel, QQmlEngine::contextForObject(this));

	auto* old = qobject_cast<WinPanelInterface*>(oldInstance);
	this->panel->reload(old != nullptr ? old->panel : nullptr);
}

ProxyWindowBase* WinPanelInterface::proxyWindow() const { return this->panel; }

// NOLINTBEGIN
#define proxyPair(type, get, set)                                                                  \
	type WinPanelInterface::get() const { return this->panel->get(); }                               \
	void WinPanelInterface::set(type value) { this->panel->set(value); }

proxyPair(Anchors, anchors, setAnchors);
proxyPair(Margins, margins, setMargins);
proxyPair(qint32, exclusiveZone, setExclusiveZone);
proxyPair(ExclusionMode::Enum, exclusionMode, setExclusionMode);
proxyPair(bool, focusable, setFocusable);
proxyPair(bool, aboveWindows, setAboveWindows);

#undef proxyPair
// NOLINTEND

} // namespace qs::windows
