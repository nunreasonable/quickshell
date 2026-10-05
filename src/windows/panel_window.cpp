#include "panel_window.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

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
#include <qtenvironmentvariables.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <shellapi.h>

#include "../core/qmlscreen.hpp"
#include "../core/types.hpp"
#include "../window/panelinterface.hpp"
#include "../window/proxywindow.hpp"
#include "appbar.hpp"
#include "blur.hpp"
#include "desktop_host.hpp"
#include "input_mask.hpp"
#include "util.hpp"
#include "virtual_desktops.hpp"

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logPanel, "quickshell.windows.panel", QtWarningMsg);

// SetParent leaves WS_CHILD / WS_POPUP alone; the window has to be switched by hand.
void setChildStyle(HWND hwnd, bool child) {
	auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
	auto newStyle = child ? ((style & ~static_cast<DWORD>(WS_POPUP)) | WS_CHILD)
	                      : ((style & ~static_cast<DWORD>(WS_CHILD)) | WS_POPUP);
	if (newStyle != style) SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(newStyle));
}

// Diagnostics for Windows 11 24H2 and later, whose desktop windows have no redirection surface:
// windows drawn through GDI only show up in there when layered. Qt draws through DXGI, which
// shouldn't need it.
bool forceLayeredDesktopPanels() {
	static const bool force = qEnvironmentVariableIntValue("QS_DESKTOP_LAYERED") != 0;
	return force;
}

// Explorer restacking the icons list over a panel above the icons on every restack of ours would
// be a fight; past this many restacks in RESTACK_WINDOW_MS the panel stays where it is.
constexpr int MAX_RESTACKS = 20;
constexpr DWORD RESTACK_WINDOW_MS = 5000;

// A region in logical window coordinates as physical pixels, rounded outwards.
QRegion toPhysicalRegion(const QRegion& region, qreal dpr) {
	QRegion physical;

	for (const auto& rect: region) {
		auto left = static_cast<int>(std::floor(rect.x() * dpr));
		auto top = static_cast<int>(std::floor(rect.y() * dpr));
		auto right = static_cast<int>(std::ceil((rect.x() + rect.width()) * dpr));
		auto bottom = static_cast<int>(std::ceil((rect.y() + rect.height()) * dpr));
		physical += QRect(left, top, right - left, bottom - top);
	}

	return physical;
}

HRGN toHrgn(const QRegion& region) {
	auto* hrgn = CreateRectRgn(0, 0, 0, 0);

	for (const auto& rect: region) {
		auto* part = CreateRectRgn(rect.x(), rect.y(), rect.x() + rect.width(), rect.y() + rect.height());
		CombineRgn(hrgn, hrgn, part, RGN_OR);
		DeleteObject(part);
	}

	return hrgn;
}
} // namespace

// Every visible panel, for operations that span windows: keeping Overlay panels above Top
// panels and lowering Top panels while a fullscreen application is active.
class WinPanelStack {
public:
	static WinPanelStack* instance() {
		static WinPanelStack* stack = nullptr; // NOLINT

		if (stack == nullptr) {
			stack = new WinPanelStack();

			QObject::connect(
			    VirtualDesktops::instance(),
			    &VirtualDesktops::currentChanged,
			    VirtualDesktops::instance(),
			    [] { stack->followCurrentDesktop(); }
			);
		}

		return stack;
	}

	// Panels belong to no virtual desktop, like the taskbar. Ones the shell refused to pin
	// (no VirtualDesktopAccessor.dll, or an unsupported build) are moved along instead, with the
	// documented IVirtualDesktopManager.
	void followCurrentDesktop() {
		auto* desktops = VirtualDesktops::instance();

		for (auto* panel: this->mPanels) {
			// Desktop panels are on every desktop with the desktop itself.
			if (panel->pinnedToAllDesktops || panel->isEmbedded()) continue;
			auto* hwnd = panel->hwnd();
			if (hwnd == nullptr || desktops->isWindowOnCurrent(hwnd)) continue;
			desktops->moveWindow(hwnd, desktops->currentIndex());
		}
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

	// Background panels go below Bottom panels, and both below every application window.
	// HWND_BOTTOM puts a window under all others, so backgrounds are re-lowered after a Bottom
	// panel was placed.
	void lowerBackgrounds() {
		for (auto* panel: this->mPanels) {
			if (panel->bLayer != PanelLayer::Background || panel->isEmbedded()) continue;
			auto* hwnd = panel->hwnd();
			if (hwnd == nullptr) continue;
			SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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

void WinProxiedWindow::setPanel(WinPanelWindow* panel) { this->mPanel = panel; }

bool WinProxiedWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
	if (eventType == "windows_generic_MSG" && !this->mPanel.isNull()) {
		if (this->mPanel->handleNativeMessage(static_cast<MSG*>(message), result)) return true;
	}

	return this->ProxiedWindow::nativeEvent(eventType, message, result);
}

// WinPanelWindow

WinPanelWindow::WinPanelWindow(QObject* parent)
    : ProxyWindowBase(parent)
    , blur(new PanelBlur(this)) {
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

	// Queued: lookups run from inside updateLayer, which must not re-enter itself.
	auto* host = DesktopHost::instance();
	auto queued = Qt::QueuedConnection;
	QObject::connect(
	    host,
	    &DesktopHost::parentChanged,
	    this,
	    [this] {
		    // A fresh lookup result: a handle refused earlier may since have been reused by an
		    // unrelated window (handles are small integers recycled quickly), so don't let a
		    // coincidental match keep refusing it forever.
		    this->embedRefusedBy = nullptr;
		    this->updateLayer();
	    },
	    queued
	);
	QObject::connect(host, &DesktopHost::parentMoved, this, &WinPanelWindow::placeEmbedded, queued);
	QObject::connect(
	    host,
	    &DesktopHost::aboveIconsChanged,
	    this,
	    &WinPanelWindow::updateLayer,
	    queued
	);
	QObject::connect(host, &DesktopHost::iconsRestacked, this, &WinPanelWindow::keepAboveIcons);
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
	// Same for the blur backdrop, which would otherwise flicker.
	this->blur->adopt(old->blur);
	// And the window stays inside the desktop.
	this->mEmbedParent = old->mEmbedParent;
	this->mEmbedInsertAfter = old->mEmbedInsertAfter;
	this->mEmbedAboveIcons = old->mEmbedAboveIcons;
	this->mEmbedRect = old->mEmbedRect;
	// Along with the region it carries, until this panel's mask comes.
	this->mInputMask = old->mInputMask;
	this->mHasInputMask = old->mHasInputMask;
	this->mAppliedRegion = old->mAppliedRegion;
	this->mRegionApplied = old->mRegionApplied;

	return old->disownWindow();
}

ProxiedWindow* WinPanelWindow::createQQuickWindow() { return new WinProxiedWindow(this); }

void WinPanelWindow::connectWindow() {
	this->ProxyWindowBase::connectWindow();

	if (auto* window = qobject_cast<WinProxiedWindow*>(this->window)) {
		window->setPanel(this);

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
	QObject::connect(this->window, &QWindow::screenChanged, this, &WinPanelWindow::onWindowScreenChanged);
	QObject::connect(this->window, &QWindow::screenChanged, this, &WinPanelWindow::updateScreen);
	QObject::connect(this->window, &ProxiedWindow::devicePixelRatioChanged, this, &WinPanelWindow::scheduleUpdateDimensions);
	// clang-format on

	this->updateScreen();

	// A window kept across a reload is still inside the desktop (a new one is not).
	if (this->hwnd() == nullptr) {
		this->mEmbedParent = nullptr;
		this->mEmbedInsertAfter = nullptr;
		this->mEmbedAboveIcons = false;
		this->mRegionApplied = false;
		this->mAppliedRegion = QRegion();
	}

	// Qt::Tool gives WS_EX_TOOLWINDOW: no taskbar button, no alt-tab entry, and the shell shows
	// such windows on every virtual desktop, which is what a panel wants. (Real pinning through
	// the virtual desktop interfaces is a later task.) Frameless: WS_POPUP without a caption.
	this->window->setFlags(Qt::Tool | Qt::FramelessWindowHint);
	// Qt just rewrote the style of the window it takes for a top level one.
	if (this->hwnd() != nullptr) this->applyNativeStyles();
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

		if (auto* window = qobject_cast<WinProxiedWindow*>(this->window)) {
			window->setPanel(nullptr);
		}
	}

	this->appBar.remove();
	this->blur->release();
	WinPanelStack::instance()->removePanel(this);

	// The window itself stays where it is: it either goes to the next panel (reload) or is
	// destroyed with its parent's region repainted by explorer.
	this->mEmbedParent = nullptr;
	this->mEmbedInsertAfter = nullptr;
	this->mEmbedAboveIcons = false;
	this->mRegionApplied = false;
	this->mButtonHeld = false;
	this->destroyedHwnd = nullptr;
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

	// A window reused across a reload is already visible and emits no visibleChanged.
	if (this->window->isVisible()) {
		WinPanelStack::instance()->addPanel(this);
		this->stickToAllDesktops();
	}

	applyPanelDwmAttributes(this->hwnd());
	this->applyNativeStyles();
	this->updateDimensions();
	this->updateLayer();
	this->updateFocus();
	this->blur->attach();
}

void WinPanelWindow::applyNativeStyles() {
	auto* hwnd = this->hwnd();
	// Qt::Tool already requests WS_EX_TOOLWINDOW; enforce it in case the flag mapping changes.
	setExStyleBits(hwnd, WS_EX_TOOLWINDOW, true);

	if (this->mEmbedParent != nullptr) {
		// Qt rewrites the styles of what it takes for a top level window as a popup's.
		setChildStyle(hwnd, true);
		// Clicks and the window's destruction aren't reported to explorer's windows: they don't
		// expect a child they didn't create.
		setExStyleBits(hwnd, WS_EX_NOPARENTNOTIFY, true);

		if (forceLayeredDesktopPanels()) {
			setExStyleBits(hwnd, WS_EX_LAYERED, true);
			SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
		}
	}
}

void WinPanelWindow::stickToAllDesktops() {
	auto* hwnd = this->hwnd();
	// The virtual desktop manager only tracks top level windows.
	if (hwnd == nullptr || this->pinnedToAllDesktops || this->mEmbedParent != nullptr) return;

	auto* desktops = VirtualDesktops::instance();

	// Shown on whatever desktop is current; it may have been created on another one.
	if (!desktops->isWindowOnCurrent(hwnd)) desktops->moveWindow(hwnd, desktops->currentIndex());

	// The shell only knows a window (and can pin it) once it was shown, so pin on the next turn.
	QTimer::singleShot(0, this, [this] {
		auto* hwnd = this->hwnd();
		if (hwnd == nullptr || !this->window->isVisible() || this->pinnedToAllDesktops
		    || this->mEmbedParent != nullptr)
			return;

		auto* desktops = VirtualDesktops::instance();
		this->pinnedToAllDesktops = desktops->isWindowPinned(hwnd) || desktops->pinWindow(hwnd, true);

		if (!this->pinnedToAllDesktops) {
			qCDebug(logPanel) << "Could not pin" << this << "to all desktops; it will follow the current one";
		}
	});
}

void WinPanelWindow::onWindowVisibleChanged() {
	if (this->window->isVisible()) {
		WinPanelStack::instance()->addPanel(this);
		this->updateDimensions();
		this->updateLayer();
		this->stickToAllDesktops();

		if (this->bKeyboardFocus == PanelKeyboardFocus::Exclusive) {
			this->grabKeyboardFocus();
		} else if (this->bKeyboardFocus == PanelKeyboardFocus::OnDemand) {
			QTimer::singleShot(50, this, [this]() {
				if (this->window == nullptr || !this->isVisibleDirect()
				    || this->bKeyboardFocus != PanelKeyboardFocus::OnDemand)
					return;

				qCDebug(logPanel) << "Taking keyboard focus for" << this << "as it is shown";
				this->grabKeyboardFocus();
			});
		}
	} else {
		// Hidden AppBars still reserve space, so drop the reservation with the window.
		this->appBar.remove();
		WinPanelStack::instance()->removePanel(this);
	}

	this->blur->syncPlacement();
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
	// Hidden panels must not reserve space. Without the visibility check, the work area change
	// caused by removing the reservation on hide schedules this function again and re-reserves.
	auto wantsAppBar = !ignoreZones && edge != 0 && zone > 0 && hwnd != nullptr && rects.valid
	                && this->window->isVisible();

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

	if (this->mEmbedParent != nullptr) this->mEmbedRect = mapper.toPhysical(geometry);

	if (this->window->geometry() != geometry) {
		qCDebug(logPanel) << "Placing" << this << "at" << geometry << "on" << screen->name();
		this->window->setGeometry(geometry);
	}

	if (this->mEmbedParent != nullptr) {
		// Also when only the parent moved, which Qt doesn't know about.
		this->placeEmbedded();
		// Started reserving space: a desktop panel can't.
		if (!this->wantsEmbedding()) this->updateLayer();
	}
}

void WinPanelWindow::updateLayer() {
	if (this->window == nullptr) return;

	auto layer = this->bLayer.value();
	auto* hwnd = this->hwnd();

	// Before the flag change below: Qt restyles what it takes for a top level window, so a
	// panel leaving the desktop (e.g. to Overlay) has to be one again by then.
	if (hwnd != nullptr) this->updateEmbedding();

	// Qt keeps WS_EX_TOPMOST in sync with this flag across its own style rewrites.
	this->window->setFlag(Qt::WindowStaysOnTopHint, layer >= PanelLayer::Top);

	if (hwnd == nullptr) return;

	this->applyNativeStyles();

	if (this->mEmbedParent != nullptr) {
		// The z order inside the desktop is fixed by placeEmbedded.
		InputMaskTracker::instance()->refresh();
		return;
	}

	HWND insertAfter = nullptr;
	switch (layer) {
	// Progman (the desktop) is kept bottom-most by the window manager, so HWND_BOTTOM lands
	// directly above it. Like on a compositor, Bottom panels stay under application windows too
	// (ii's wallpaper and desktop widgets are one); WM_WINDOWPOSCHANGING keeps both down there.
	case PanelLayer::Background:
	case PanelLayer::Bottom: insertAfter = HWND_BOTTOM; break;
	// Like the taskbar: drop out of the topmost band while a fullscreen app is active.
	case PanelLayer::Top:
		insertAfter = WinPanelStack::instance()->fullscreenAppActive() ? HWND_BOTTOM : HWND_TOPMOST;
		break;
	case PanelLayer::Overlay: insertAfter = HWND_TOPMOST; break;
	}

	SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	if (layer == PanelLayer::Bottom) WinPanelStack::instance()->lowerBackgrounds();
	if (layer != PanelLayer::Overlay) WinPanelStack::instance()->raiseOverlays();

	// style rewrites by qt drop WS_EX_TRANSPARENT
	InputMaskTracker::instance()->refresh();
}

bool WinPanelWindow::reservesSpace() const {
	auto mode = this->bExclusionMode.value();
	if (mode == ExclusionMode::Ignore) return false;
	if (mode == ExclusionMode::Normal && this->bExclusiveZone.value() < 0) return false;
	return this->bcExclusionEdge.value() != 0 && this->bcExclusiveZone.value() > 0;
}

bool WinPanelWindow::wantsEmbedding() const {
	// Wallpapers and desktop widgets. Anything that takes keyboard focus, or pushes windows
	// away, stays a window.
	return DesktopHost::instance()->enabled() && this->bLayer.value() <= PanelLayer::Bottom
	    && this->bKeyboardFocus.value() == PanelKeyboardFocus::None && !this->reservesSpace();
}

bool WinPanelWindow::wantsAboveIcons() const {
	// The icons view covers the desktop behind it and takes every click there, so desktop
	// widgets that take the mouse go above the icons, by namespace like a compositor's rules.
	return DesktopHost::instance()->aboveIcons().contains(this->bNamespace.value());
}

void WinPanelWindow::updateEmbedding() {
	auto* hwnd = this->hwnd();
	if (hwnd == nullptr) return;

	// Not where it was put: nothing left to undo.
	if (this->mEmbedParent != nullptr && GetAncestor(hwnd, GA_PARENT) != this->mEmbedParent) {
		this->mEmbedParent = nullptr;
		this->mEmbedInsertAfter = nullptr;
		this->mEmbedAboveIcons = false;
	}

	auto* host = DesktopHost::instance();
	auto embed = this->wantsEmbedding();
	auto aboveIcons = embed && this->wantsAboveIcons();

	HWND parent = nullptr;
	// Without an icons view, a panel that takes input is better off as a window than out of
	// reach behind the icons.
	if (embed) parent = aboveIcons ? host->iconsHost() : host->parentWindow();
	// Refused once (DPI awareness or integrity mismatch): it would be refused again.
	if (parent != nullptr && parent == this->embedRefusedBy) parent = nullptr;
	if (parent == nullptr) aboveIcons = false;
	auto* insertAfter = parent == nullptr || aboveIcons ? nullptr : host->insertAfter();

	if (parent == this->mEmbedParent && insertAfter == this->mEmbedInsertAfter
	    && aboveIcons == this->mEmbedAboveIcons)
	{
		return;
	}

	if (parent != nullptr) this->embedInto(parent, insertAfter, aboveIcons);
	else this->unembed();
}

void WinPanelWindow::embedInto(HWND parent, HWND insertAfter, bool aboveIcons) {
	auto* hwnd = this->hwnd();
	auto wasEmbedded = this->mEmbedParent != nullptr;

	if (!wasEmbedded) {
		// Placed by updateDimensions as a top level window: the same spot on screen.
		RECT rect {};
		if (GetWindowRect(hwnd, &rect)) this->mEmbedRect = toQRect(rect);
		// Before SetParent, as documented, so it is never a popup with a parent.
		setChildStyle(hwnd, true);
	}

	// Set first: SetParent's own messages already go to a child.
	this->mEmbedParent = parent;
	this->mEmbedInsertAfter = insertAfter;
	this->mEmbedAboveIcons = aboveIcons;

	SetLastError(0);
	if (SetParent(hwnd, parent) == nullptr && GetLastError() != 0) {
		qCWarning(logPanel) << "Could not put" << this << "into the desktop, error" << GetLastError()
		                    << "- it stays a window";
		this->embedRefusedBy = parent;
		// Whatever it was before (a top level window, or inside the previous desktop window).
		if (wasEmbedded) this->unembed();
		else {
			this->mEmbedParent = nullptr;
			this->mEmbedInsertAfter = nullptr;
			this->mEmbedAboveIcons = false;
			setChildStyle(hwnd, false);
		}
		return;
	}

	qCDebug(logPanel) << "Put" << this << "into the desktop window" << parent
	                  << (aboveIcons ? "above the icons" : "behind the icons");

	this->embedRefusedBy = nullptr;
	this->restackCount = 0;
	// Left by the virtual desktop manager along with the top level window list.
	this->pinnedToAllDesktops = false;
	this->appBar.remove();
	this->applyNativeStyles();
	this->placeEmbedded();
	this->routeInputMask();
	// The blur backdrop is a top level window behind the panel: off inside the desktop.
	this->blur->attach();
}

void WinPanelWindow::unembed() {
	auto* hwnd = this->hwnd();
	auto wasEmbedded = this->mEmbedParent != nullptr;
	this->mEmbedParent = nullptr;
	this->mEmbedInsertAfter = nullptr;
	this->mEmbedAboveIcons = false;
	this->mButtonHeld = false;
	if (hwnd == nullptr || !wasEmbedded) return;

	SetParent(hwnd, nullptr);
	setChildStyle(hwnd, false);
	setExStyleBits(hwnd, WS_EX_NOPARENTNOTIFY, false);
	if (forceLayeredDesktopPanels()) setExStyleBits(hwnd, WS_EX_LAYERED, false);
	// The input mask goes back to click-through toggling, without a window region.
	this->routeInputMask();

	// The same spot on screen, now in screen coordinates. The caller restacks it.
	UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED;
	auto rect = this->mEmbedRect;
	if (!rect.isValid()) flags |= SWP_NOMOVE | SWP_NOSIZE;
	SetWindowPos(hwnd, nullptr, rect.x(), rect.y(), rect.width(), rect.height(), flags);

	qCDebug(logPanel) << "Took" << this << "out of the desktop";

	this->blur->attach();
	if (this->window->isVisible()) this->stickToAllDesktops();
}

QRect WinPanelWindow::embeddedRect() const {
	// Two points are mapped as a rect, so a mirrored (RTL) parent swaps the edges correctly.
	auto rect = toRECT(this->mEmbedRect);
	MapWindowPoints(HWND_DESKTOP, this->mEmbedParent, reinterpret_cast<POINT*>(&rect), 2); // NOLINT
	return toQRect(rect);
}

void WinPanelWindow::placeEmbedded() {
	auto* hwnd = this->hwnd();
	if (hwnd == nullptr || this->mEmbedParent == nullptr || !this->mEmbedRect.isValid()) return;

	auto rect = this->embeddedRect();
	UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
	// Inside Progman, the icons view is a sibling and explorer may restack its children. Above
	// the icons, it is a sibling too.
	auto* insertAfter = this->mEmbedAboveIcons ? HWND_TOP : this->mEmbedInsertAfter;
	if (insertAfter == nullptr && !this->mEmbedAboveIcons) flags |= SWP_NOZORDER;

	this->placingEmbedded = true;
	SetWindowPos(
	    hwnd,
	    insertAfter,
	    rect.x(),
	    rect.y(),
	    rect.width(),
	    rect.height(),
	    flags
	);
	this->placingEmbedded = false;

	// The device pixel ratio may have changed with the screen.
	if (this->mEmbedAboveIcons) this->applyInputRegion();
}

void WinPanelWindow::keepAboveIcons() {
	auto* hwnd = this->hwnd();
	if (hwnd == nullptr || !this->mEmbedAboveIcons) return;

	auto covered = false;
	for (auto* above = GetWindow(hwnd, GW_HWNDPREV); above != nullptr;
	     above = GetWindow(above, GW_HWNDPREV))
	{
		// Other panels above the icons may come first, in any order.
		if (IsWindowVisible(above) && !isOwnProcessWindow(above)) {
			covered = true;
			break;
		}
	}

	if (!covered) return;

	auto now = GetTickCount();
	if (this->restackCount == 0 || now - this->restackWindowStart > RESTACK_WINDOW_MS) {
		this->restackCount = 0;
		this->restackWindowStart = now;
	}

	if (++this->restackCount > MAX_RESTACKS) {
		if (this->restackCount == MAX_RESTACKS + 1) {
			qCWarning(logPanel) << "Explorer keeps covering" << this
			                    << "with the desktop icons; leaving it there";
		}

		return;
	}

	qCDebug(logPanel) << "Putting" << this << "back above the desktop icons";
	this->placeEmbedded();
}

void WinPanelWindow::routeInputMask() {
	if (this->window == nullptr) return;
	auto* tracker = InputMaskTracker::instance();

	// Hit testing only passes a child window by (click-through) to windows of the same thread,
	// never to explorer's icons below it, so above the icons the mask is the window region
	// instead. That also clips what is drawn outside it.
	if (this->mHasInputMask && !this->mEmbedAboveIcons) {
		tracker->setMask(this->window, this->mInputMask);
	} else {
		tracker->remove(this->window);
	}

	this->applyInputRegion();
}

void WinPanelWindow::applyInputRegion() {
	auto* hwnd = this->hwnd();
	if (hwnd == nullptr || this->window == nullptr) return;

	// Lifted while a button is held: the window has the mouse captured anyway, and a widget being
	// dragged isn't clipped by a region that lags a frame behind it.
	auto wantsRegion = this->mEmbedAboveIcons && this->mHasInputMask && !this->mButtonHeld;
	auto region = wantsRegion ? toPhysicalRegion(this->mInputMask, this->window->devicePixelRatio())
	                          : QRegion();

	if (wantsRegion == this->mRegionApplied && region == this->mAppliedRegion) return;

	// An empty region is a window without input and without anything drawn, like an empty mask.
	auto* hrgn = wantsRegion ? toHrgn(region) : nullptr;

	// the system owns the region from here on
	if (SetWindowRgn(hwnd, hrgn, TRUE) == 0) {
		if (hrgn != nullptr) DeleteObject(hrgn);
		qCWarning(logPanel) << "Could not set the window region of" << this;
		return;
	}

	this->mRegionApplied = wantsRegion;
	this->mAppliedRegion = region;
}

void WinPanelWindow::setButtonHeld(bool held) {
	if (held == this->mButtonHeld) return;
	this->mButtonHeld = held;
	this->applyInputRegion();
}

void WinPanelWindow::onWindowScreenChanged() {
	// Qt takes a child window's screen from its top level ancestor, the desktop window spanning
	// every monitor, and switches it on geometry and DPI changes. The panel is placed on its
	// own screen regardless, but QML reads `screen` and Qt the DPI from it: put it back.
	// mTrackedScreen (not the base class's mScreen, which stays null unless QML sets `screen:`
	// explicitly) is the screen updateDimensions() actually placed this panel on.
	if (this->mEmbedParent == nullptr || this->mTrackedScreen == nullptr || this->screenRestorePending)
	{
		return;
	}

	if (this->window->screen() == this->mTrackedScreen) return;
	this->screenRestorePending = true;

	QTimer::singleShot(0, this, [this] {
		this->screenRestorePending = false;

		if (this->window != nullptr && this->mEmbedParent != nullptr && this->mTrackedScreen != nullptr
		    && this->window->screen() != this->mTrackedScreen)
		{
			// Screens of one virtual desktop: the window is neither moved nor recreated.
			this->window->setScreen(this->mTrackedScreen);
		}
	});
}

void WinPanelWindow::recreateDestroyedWindow() {
	auto* dead = std::exchange(this->destroyedHwnd, nullptr);
	// Qt clears its own handle once it has processed the WM_DESTROY (hwnd() goes back to
	// nullptr), so the dead handle can't be matched against hwnd() here: that check never
	// passed, leaving the panel stuck. A reload in between already cleared `dead` above, via
	// releaseNativeState(), so there's nothing left to guard against but the handle lingering.
	if (dead == nullptr || this->window == nullptr || IsWindow(dead)) return;

	qCInfo(logPanel) << "The desktop went away with" << this << "inside, recreating its window";

	auto visible = this->visibleWhenDestroyed;
	this->deleteWindow();
	this->createWindow();
	// Comes back as a top level window; explorer's TaskbarCreated puts it into the new desktop.
	if (visible) this->setVisible(true);
}

void WinPanelWindow::updateFocus() {
	if (this->window == nullptr) return;

	auto focus = this->bKeyboardFocus.value();

	// Desktop panels take no focus: one that does leaves the desktop (before Qt restyles it),
	// one that stopped may join it.
	if (this->hwnd() != nullptr && this->wantsEmbedding() != (this->mEmbedParent != nullptr)) {
		this->updateLayer();
	}

	// WS_EX_NOACTIVATE: the window receives mouse input but never becomes the active window,
	// so the application underneath keeps keyboard focus.
	this->window->setFlag(Qt::WindowDoesNotAcceptFocus, focus == PanelKeyboardFocus::None);
	if (this->hwnd() != nullptr) this->applyNativeStyles();

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

	this->mInputMask = region;
	this->mHasInputMask = hasMask;
	this->routeInputMask();

	this->blur->setInputMask(region, hasMask);
}

bool WinPanelWindow::handleNativeMessage(MSG* msg, qintptr* result) {
	if (this->window == nullptr) return false;

	if (this->mEmbedParent != nullptr) {
		switch (msg->message) {
		case WM_WINDOWPOSCHANGING: {
			auto* pos = reinterpret_cast<WINDOWPOS*>(msg->lParam); // NOLINT(performance-no-int-to-ptr)

			// Qt places the window in screen coordinates and a child is placed in its parent's;
			// also Qt's own idea of the screen comes from the parent, which spans all monitors.
			// A desktop panel only ever goes where updateDimensions put it.
			if (this->mEmbedRect.isValid()) {
				auto rect = this->embeddedRect();

				if (!(pos->flags & SWP_NOMOVE)) {
					pos->x = rect.x();
					pos->y = rect.y();
				}

				if (!(pos->flags & SWP_NOSIZE)) {
					pos->cx = rect.width();
					pos->cy = rect.height();
				}
			}

			// Raising it (showing, Qt's restyling) would cover the icons.
			if (!this->placingEmbedded) pos->flags |= SWP_NOZORDER;

			// Kept from Qt: it takes a z order change of a window with a foreign parent for
			// someone else embedding it, and from then on reports its geometry relative to the
			// parent, which no longer matches the screen coordinates it places it in.
			*result = 0;
			return true;
		}
		case WM_LBUTTONDOWN:
		case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONDBLCLK:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONDBLCLK:
			if (this->mEmbedAboveIcons) this->setButtonHeld(true);
			break;
		// Before Qt handles the release, so the capture Qt took on the press is still there; its
		// release comes as WM_CAPTURECHANGED.
		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
		case WM_MBUTTONUP:
		case WM_XBUTTONUP:
			if (GetCapture() != msg->hwnd) this->setButtonHeld(false);
			break;
		case WM_CAPTURECHANGED:
			if (reinterpret_cast<HWND>(msg->lParam) != msg->hwnd) this->setButtonHeld(false); // NOLINT
			break;
		case WM_DESTROY:
			// Explorer went away (crash, restart) and took its desktop's child windows, this one
			// included. Qt would keep rendering to the dead handle, so the window is replaced.
			this->mEmbedParent = nullptr;
			this->mEmbedInsertAfter = nullptr;
			this->mEmbedAboveIcons = false;
			this->mRegionApplied = false;
			this->mAppliedRegion = QRegion();
			this->mButtonHeld = false;
			this->destroyedHwnd = msg->hwnd;
			this->visibleWhenDestroyed = this->window->isVisible();
			QTimer::singleShot(0, this, &WinPanelWindow::recreateDestroyedWindow);
			return false;
		default: break;
		}
	}

	if (msg->message == WinAppBar::callbackMessage()) {
		switch (msg->wParam) {
		case ABN_POSCHANGED: this->scheduleUpdateDimensions(); break;
		case ABN_FULLSCREENAPP: {
			// One of our own surfaces is never the fullscreen app, even where NonRudeHWND
			// didn't stick (it is only read when explorer first sees the window).
			auto active = msg->lParam != 0;
			if (active && isOwnProcessWindow(GetForegroundWindow())) active = false;
			WinPanelStack::instance()->setFullscreenAppActive(active);
			break;
		}
		default: break;
		}

		*result = 0;
		return true;
	}

	if (msg->message == WinAppBar::taskbarCreatedMessage()) {
		// Explorer restarted: all AppBar registrations and the work area are gone.
		this->appBar.invalidate();
		this->scheduleUpdateDimensions();
		this->blur->syncPlacement();
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
		// moved, resized, restacked, shown or hidden: the blur backdrop follows
		this->blur->syncPlacement();
		break;
	// Transparency effects, high contrast and energy saver turn blur off. Every top level window
	// gets these broadcasts; the manager coalesces them.
	case WM_SETTINGCHANGE:
	case WM_THEMECHANGED:
	case WM_SYSCOLORCHANGE:
		if (auto* manager = BlurManager::instance()) manager->scheduleSystemCheck();
		break;
	case WM_POWERBROADCAST:
		if (msg->wParam == PBT_APMPOWERSTATUSCHANGE) {
			if (auto* manager = BlurManager::instance()) manager->scheduleSystemCheck();
		}
		break;
	// DWM restarted: composition targets may be gone.
	case WM_DWMCOMPOSITIONCHANGED:
		if (auto* manager = BlurManager::instance()) manager->scheduleSystemCheck(true);
		break;
	case WM_WINDOWPOSCHANGING:
		if (this->bLayer == PanelLayer::Background || this->bLayer == PanelLayer::Bottom) {
			auto* pos = reinterpret_cast<WINDOWPOS*>(msg->lParam); // NOLINT(performance-no-int-to-ptr)
			// Activation and qt's style rewrites try to raise the window; keep it on the desktop.
			// Only updateLayer() moves it, always to HWND_BOTTOM.
			if (!(pos->flags & SWP_NOZORDER) && pos->hwndInsertAfter != HWND_BOTTOM) {
				if (this->bLayer == PanelLayer::Background) pos->hwndInsertAfter = HWND_BOTTOM;
				else pos->flags |= SWP_NOZORDER;
			}
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
