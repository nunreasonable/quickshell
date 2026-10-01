#pragma once

#include <qt_windows.h>

#include <qrect.h>
#include <qtclasshelpermacros.h>
#include <qtypes.h>

namespace qs::windows {

// Win32 AppBar registration for one window. An AppBar reserves a strip along a monitor edge
// in the system work area, like a wlr-layer-shell exclusive zone. All rects are physical pixels.
class WinAppBar {
public:
	WinAppBar() = default;
	~WinAppBar() { this->remove(); }
	Q_DISABLE_COPY_MOVE(WinAppBar);

	// Message id the shell sends ABN_* notifications with (wParam = ABN_*, lParam = data).
	static UINT callbackMessage();
	// Broadcast by explorer after it (re)starts; every AppBar has to register again.
	static UINT taskbarCreatedMessage();

	[[nodiscard]] bool registered() const { return this->mRegistered; }
	[[nodiscard]] HWND hwnd() const { return this->mHwnd; }
	[[nodiscard]] QRect reservedRect() const { return this->mReserved; }

	// Reserves `size` pixels along `edge` (ABE_*) of `monitor`, registering the AppBar first
	// if needed. Returns the rect the shell granted, which may be pushed inwards by the taskbar
	// or other AppBars, or an invalid rect if the shell refused.
	QRect reserve(HWND hwnd, UINT edge, const QRect& monitor, qint32 size);

	// Unregisters the AppBar, releasing the reserved area.
	void remove();

	// Forgets the registration without talking to the shell (explorer restarted, hwnd gone).
	void invalidate();

	// Takes over another object's registration (same HWND, new owner) without a round trip
	// to the shell, so the reserved area does not flicker.
	void adopt(WinAppBar& other);

	void notifyActivate();
	void notifyWindowPosChanged();

private:
	bool ensureRegistered(HWND hwnd);

	HWND mHwnd = nullptr;
	bool mRegistered = false;
	UINT mEdge = 0;
	QRect mReserved;
};

} // namespace qs::windows
