#pragma once

#include <qt_windows.h>

#include <qpoint.h>
#include <qrect.h>
#include <qscreen.h>
#include <qtypes.h>
#include <qwindow.h>

namespace qs::windows {

// Physical (device pixel) rectangles of a monitor, from GetMonitorInfo.
// The work area already excludes the taskbar and every registered AppBar, including ours.
struct MonitorRects {
	QRect monitor;
	QRect work;
	bool valid = false;
};

[[nodiscard]] HMONITOR monitorForScreen(QScreen* screen);
[[nodiscard]] MonitorRects monitorRects(HMONITOR monitor);

// Converts between Qt's logical screen coordinates and physical pixels of a single monitor.
// Qt positions a screen's logical rect at the monitor's physical origin, so inside one monitor
// the mapping is a translation plus the screen's device pixel ratio.
class ScreenMapper {
public:
	ScreenMapper(const QRect& logicalScreen, const QRect& physicalMonitor, qreal dpr)
	    : logicalOrigin(logicalScreen.topLeft())
	    , physicalOrigin(physicalMonitor.topLeft())
	    , dpr(dpr) {}

	[[nodiscard]] QPoint toLogical(const QPoint& physical) const;
	[[nodiscard]] QRect toLogical(const QRect& physical) const;
	[[nodiscard]] QPoint toPhysical(const QPoint& logical) const;
	[[nodiscard]] QRect toPhysical(const QRect& logical) const;
	[[nodiscard]] qint32 toPhysical(qint32 logicalLength) const;

private:
	QPoint logicalOrigin;
	QPoint physicalOrigin;
	qreal dpr;
};

[[nodiscard]] HWND hwndOf(const QWindow* window);
[[nodiscard]] QRect toQRect(const RECT& rect);
[[nodiscard]] RECT toRECT(const QRect& rect);

// SetForegroundWindow refuses to steal focus from another process unless our thread is the
// foreground thread, so temporarily share input state with it (the AttachThreadInput trick).
bool forceForegroundWindow(HWND hwnd);

// True if the window belongs to this process (used to avoid fighting our own popups for focus).
[[nodiscard]] bool isOwnProcessWindow(HWND hwnd);

// Keeps explorer from treating the window as a fullscreen app (see util.cpp).
void markNonRude(HWND hwnd);

// Square corners, no DWM border/shadow, no animations and no Aero Peek hiding: makes the
// window look like a shell surface instead of an application window.
void applyPanelDwmAttributes(HWND hwnd);

// Adds or removes bits of the extended window style without touching the others.
void setExStyleBits(HWND hwnd, LONG_PTR bits, bool enabled);

} // namespace qs::windows
