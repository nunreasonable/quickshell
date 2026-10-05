#include "item.hpp"

#include <qt_windows.h>
#include <shellapi.h>

#include <qimage.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qpixmap.h>
#include <qsize.h>
#include <qstring.h>
#include <qtypes.h>
#include <quuid.h>

#include "../../appicon.hpp"
#include "hook.hpp"

namespace qs::windows::services::systray {

namespace {
// What apps that don't set an icon get from their executable.
constexpr int FALLBACK_ICON_SIZE = 32;
} // namespace

QPixmap TrayIconImage::requestPixmap(const QString& /*id*/, QSize* size, const QSize& requestedSize) {
	auto pixmap = QPixmap::fromImage(this->image);

	// Apps hand over a single small icon sized for the taskbar's DPI.
	if (!pixmap.isNull() && requestedSize.width() > 0 && requestedSize.height() > 0
	    && pixmap.size() != requestedSize)
	{
		pixmap = pixmap.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}

	if (size != nullptr) *size = pixmap.size();
	return pixmap;
}

SystemTrayItem::SystemTrayItem(HWND hwnd, UINT uid, const QUuid& guid, QObject* parent)
    : QObject(parent)
    , hwnd(hwnd)
    , uid(uid)
    , guid(guid) {}

bool SystemTrayItem::matches(HWND hwnd, UINT uid, const QUuid& guid) const {
	// With NIF_GUID the GUID alone names the icon; without it, the window and id do.
	if (!guid.isNull() && guid == this->guid) return true;
	return hwnd != nullptr && hwnd == this->hwnd && uid == this->uid;
}

bool SystemTrayItem::ownerAlive() const { return IsWindow(this->hwnd) != 0; }

void SystemTrayItem::update(const TrayIconMessage& message) {
	if (message.hwnd != nullptr) {
		this->hwnd = message.hwnd;
		this->uid = message.uid;
	}

	if ((message.flags & NIF_GUID) != 0) this->guid = message.guid;
	if ((message.flags & NIF_MESSAGE) != 0) this->callbackMessage = message.callbackMessage;
	if (message.message == NIM_SETVERSION) this->version = message.version;

	if ((message.flags & NIF_ICON) != 0) {
		this->image.image = message.icon;
		this->updateIconSource();
	}

	if ((message.flags & NIF_TIP) != 0) {
		auto tip = message.tip;
		tip.remove(u'\r');

		auto newline = tip.indexOf(u'\n');
		this->bTooltipTitle = newline == -1 ? tip : tip.first(newline);
		this->bTooltipDescription = newline == -1 ? QString() : tip.sliced(newline + 1);
	}

	if ((message.flags & NIF_STATE) != 0 && (message.stateMask & NIS_HIDDEN) != 0) {
		this->bStatus = (message.state & NIS_HIDDEN) != 0 ? Status::Passive : Status::Active;
	}
}

void SystemTrayItem::setIdentity(const QString& id, const QString& title, const QString& exePath) {
	this->bId = id;
	this->bTitle = title;
	this->exePath = exePath;
	if (this->image.image.isNull()) this->updateIconSource();
}

void SystemTrayItem::updateIconSource() {
	if (this->image.image.isNull() && !this->exePath.isEmpty()) {
		this->image.image =
		    iconForKey(this->exePath, QSize(FALLBACK_ICON_SIZE, FALLBACK_ICON_SIZE)).toImage();
	}

	if (this->image.image.isNull()) {
		this->bIcon = QString();
		return;
	}

	// A new url per change, or Image keeps the cached pixmap.
	this->image.imageChanged();
	this->bIcon = this->image.url();
}

void SystemTrayItem::allowForeground() const {
	// The app calls SetForegroundWindow before showing its menu or window, which Windows only
	// lets it do with our blessing (we got the click). Without it its menu doesn't close when
	// clicking elsewhere.
	DWORD pid = 0;
	GetWindowThreadProcessId(this->hwnd, &pid);
	if (pid != 0) AllowSetForegroundWindow(pid);
}

// The icon's callback message, in the format the app chose with NIM_SETVERSION.
void SystemTrayItem::send(UINT event) const {
	if (this->callbackMessage == 0 || !this->ownerAlive()) return;

	WPARAM wParam = 0;
	LPARAM lParam = 0;

	if (this->version >= NOTIFYICON_VERSION_4) {
		// The anchor point in screen coordinates, the event and the icon id.
		POINT cursor {};
		GetCursorPos(&cursor);
		wParam = MAKEWPARAM(static_cast<WORD>(cursor.x), static_cast<WORD>(cursor.y));
		lParam = MAKELPARAM(static_cast<WORD>(event), static_cast<WORD>(this->uid));
	} else {
		wParam = this->uid;
		lParam = event;
	}

	// Never wait on the app: some only return once their menu closes.
	SendNotifyMessageW(this->hwnd, this->callbackMessage, wParam, lParam);
}

void SystemTrayItem::activate() {
	this->allowForeground();

	// Explorer sends a double click message for the second click; some apps only open their
	// window on that.
	auto now = GetTickCount64();
	auto doubleClick = this->lastActivate != 0 && now - this->lastActivate <= GetDoubleClickTime();
	this->lastActivate = doubleClick ? 0 : now;

	this->send(doubleClick ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN);
	this->send(WM_LBUTTONUP);
	// Documented for version 4, but explorer sends it for version 3 too.
	if (this->version >= NOTIFYICON_VERSION) this->send(NIN_SELECT);
}

void SystemTrayItem::secondaryActivate() {
	this->allowForeground();
	this->send(WM_MBUTTONDOWN);
	this->send(WM_MBUTTONUP);
}

void SystemTrayItem::scroll(qint32 /*delta*/, bool /*horizontal*/) const {}

void SystemTrayItem::display(
    QObject* /*parentWindow*/,
    qint32 /*relativeX*/,
    qint32 /*relativeY*/
) {
	this->allowForeground();
	this->send(WM_RBUTTONDOWN);
	this->send(WM_RBUTTONUP);
	if (this->version >= NOTIFYICON_VERSION) this->send(WM_CONTEXTMENU);
}

} // namespace qs::windows::services::systray
