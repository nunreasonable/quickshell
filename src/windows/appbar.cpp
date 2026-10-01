#include "appbar.hpp"

#include <shellapi.h>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qrect.h>
#include <qtypes.h>

#include "util.hpp"

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logAppBar, "quickshell.windows.appbar", QtWarningMsg);

APPBARDATA appBarData(HWND hwnd) {
	APPBARDATA data {};
	data.cbSize = sizeof(data);
	data.hWnd = hwnd;
	data.uCallbackMessage = WinAppBar::callbackMessage();
	return data;
}

} // namespace

UINT WinAppBar::callbackMessage() {
	static const UINT message = RegisterWindowMessageW(L"QuickshellAppBarNotify"); // NOLINT
	return message;
}

UINT WinAppBar::taskbarCreatedMessage() {
	static const UINT message = RegisterWindowMessageW(L"TaskbarCreated"); // NOLINT
	return message;
}

bool WinAppBar::ensureRegistered(HWND hwnd) {
	if (this->mRegistered && this->mHwnd == hwnd) return true;
	if (this->mRegistered) this->remove();

	auto data = appBarData(hwnd);
	if (!SHAppBarMessage(ABM_NEW, &data)) {
		// Usually explorer is not running (yet). TaskbarCreated triggers a retry.
		qCWarning(logAppBar) << "Failed to register AppBar for window" << hwnd;
		return false;
	}

	this->mHwnd = hwnd;
	this->mRegistered = true;
	this->mReserved = QRect();
	return true;
}

QRect WinAppBar::reserve(HWND hwnd, UINT edge, const QRect& monitor, qint32 size) {
	if (hwnd == nullptr || size <= 0 || !monitor.isValid()) return {};
	if (!this->ensureRegistered(hwnd)) return {};

	auto data = appBarData(hwnd);
	data.uEdge = edge;
	data.rc = toRECT(monitor);

	// Propose the full edge strip, then let the shell push it away from the taskbar and
	// AppBars registered before us. QUERYPOS only guarantees the leading edge, so the size
	// has to be re-applied afterwards.
	auto applySize = [&]() {
		switch (edge) {
		case ABE_TOP: data.rc.bottom = data.rc.top + size; break;
		case ABE_BOTTOM: data.rc.top = data.rc.bottom - size; break;
		case ABE_LEFT: data.rc.right = data.rc.left + size; break;
		case ABE_RIGHT: data.rc.left = data.rc.right - size; break;
		default: break;
		}
	};

	applySize();
	SHAppBarMessage(ABM_QUERYPOS, &data);
	applySize();

	auto granted = toQRect(data.rc);

	if (this->mEdge != edge || this->mReserved != granted) {
		SHAppBarMessage(ABM_SETPOS, &data);
		this->mEdge = edge;
		this->mReserved = toQRect(data.rc);
		qCDebug(logAppBar) << "Reserved" << this->mReserved << "on edge" << edge << "for" << hwnd;
	}

	return this->mReserved;
}

void WinAppBar::remove() {
	if (!this->mRegistered) return;

	auto data = appBarData(this->mHwnd);
	SHAppBarMessage(ABM_REMOVE, &data);
	this->invalidate();
}

void WinAppBar::invalidate() {
	this->mRegistered = false;
	this->mHwnd = nullptr;
	this->mEdge = 0;
	this->mReserved = QRect();
}

void WinAppBar::adopt(WinAppBar& other) {
	if (&other == this) return;
	this->remove();

	this->mHwnd = other.mHwnd;
	this->mRegistered = other.mRegistered;
	this->mEdge = other.mEdge;
	this->mReserved = other.mReserved;
	other.invalidate();
}

void WinAppBar::notifyActivate() {
	if (!this->mRegistered) return;
	auto data = appBarData(this->mHwnd);
	SHAppBarMessage(ABM_ACTIVATE, &data);
}

void WinAppBar::notifyWindowPosChanged() {
	if (!this->mRegistered) return;
	auto data = appBarData(this->mHwnd);
	SHAppBarMessage(ABM_WINDOWPOSCHANGED, &data);
}

} // namespace qs::windows
