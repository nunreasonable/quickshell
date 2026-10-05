#pragma once

#include <qt_windows.h>

#include <qimage.h>
#include <qobject.h>
#include <qpixmap.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qquickimageprovider.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <quuid.h>

#include "../../../core/imageprovider.hpp"
#include "../../../core/qsmenu.hpp"

namespace qs::windows::services::systray {

struct TrayIconMessage;

///! Status of a SystemTrayItem.
/// See @@SystemTrayItem.status.
namespace Status { // NOLINT
Q_NAMESPACE;
QML_ELEMENT;

enum Enum : quint8 {
	/// Hidden by its app (NIS_HIDDEN): explorer doesn't show it either.
	Passive = 0,
	Active = 1,
	/// Never set on Windows.
	NeedsAttention = 2,
};
Q_ENUM_NS(Enum);
} // namespace Status

///! Category of a SystemTrayItem.
/// See @@SystemTrayItem.category.
namespace Category { // NOLINT
Q_NAMESPACE;
QML_ELEMENT;

enum Enum : quint8 {
	Hardware = 0,
	SystemServices = 1,
	ApplicationStatus = 2,
	Communications = 3,
};
Q_ENUM_NS(Enum);
} // namespace Category

class SystemTrayItem;

class TrayIconImage: public QsIndexedImageHandle {
public:
	explicit TrayIconImage(): QsIndexedImageHandle(QQuickImageProvider::Pixmap) {}

	QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requestedSize) override;

	QImage image;
};

///! An icon in the Windows notification area.
/// Windows backend: icons registered with `Shell_NotifyIcon`, received by a stand-in tray window
/// that passes everything on to explorer's (see hook.hpp).
///
/// Windows tray icons have no menu model: apps draw their own context menu when told the icon
/// was right clicked, which is what @@display() does. @@hasMenu is true and @@menu is always
/// null, so open the app's menu with @@display() rather than a @@Quickshell.QsMenuAnchor.
///
/// Icons of apps that were running before the shell are first known from explorer's list, without
/// the message their app listens on. The shell asks those apps to add their icons again (as after
/// an explorer restart) and reads explorer's own notification area where it can; a click that
/// comes before that is answered is held for a few seconds and delivered then.
class SystemTrayItem: public QObject {
	Q_OBJECT;
	// clang-format off
	/// The app's executable name without extension, with `:<icon id>` added for further icons
	/// of the same app.
	Q_PROPERTY(QString id READ default NOTIFY idChanged BINDABLE bindableId);
	/// The executable's description from its version information.
	Q_PROPERTY(QString title READ default NOTIFY titleChanged BINDABLE bindableTitle);
	Q_PROPERTY(qs::windows::services::systray::Status::Enum status READ default NOTIFY statusChanged BINDABLE bindableStatus);
	Q_PROPERTY(qs::windows::services::systray::Category::Enum category READ default NOTIFY categoryChanged BINDABLE bindableCategory);
	/// Icon source string, usable as an Image source.
	Q_PROPERTY(QString icon READ default NOTIFY iconChanged BINDABLE bindableIcon);
	/// First line of the icon's tooltip.
	Q_PROPERTY(QString tooltipTitle READ default NOTIFY tooltipTitleChanged BINDABLE bindableTooltipTitle);
	/// The rest of the tooltip.
	Q_PROPERTY(QString tooltipDescription READ default NOTIFY tooltipDescriptionChanged BINDABLE bindableTooltipDescription);
	Q_PROPERTY(bool hasMenu READ hasMenu NOTIFY hasMenuChanged);
	Q_PROPERTY(qs::menu::QsMenuHandle* menu READ menu NOTIFY hasMenuChanged);
	Q_PROPERTY(bool onlyMenu READ onlyMenu NOTIFY onlyMenuChanged);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("SystemTrayItems can only be acquired from SystemTray");

public:
	explicit SystemTrayItem(HWND hwnd, UINT uid, const QUuid& guid, QObject* parent = nullptr);

	/// Left click.
	Q_INVOKABLE void activate();
	/// Middle click.
	Q_INVOKABLE void secondaryActivate();
	/// Does nothing: the notification area has no scroll events.
	Q_INVOKABLE void scroll(qint32 delta, bool horizontal) const;
	/// Right click: the app opens its own context menu at the cursor. The position arguments are
	/// accepted for compatibility; apps place their menu where the click happened.
	Q_INVOKABLE void display(QObject* parentWindow, qint32 relativeX, qint32 relativeY);

	[[nodiscard]] bool matches(HWND hwnd, UINT uid, const QUuid& guid) const;
	[[nodiscard]] HWND ownerWindow() const { return this->hwnd; }
	[[nodiscard]] UINT iconId() const { return this->uid; }
	[[nodiscard]] QUuid iconGuid() const { return this->guid; }
	[[nodiscard]] bool ownerAlive() const;
	[[nodiscard]] bool hasCallback() const { return this->callbackMessage != 0; }

	// Applies an NIM_ADD/NIM_MODIFY/NIM_SETVERSION for this icon.
	void update(const TrayIconMessage& message);
	// Callback message and version read from explorer's notification area.
	void applyExplorerData(UINT callbackMessage, UINT version);
	// Icon and tooltip from explorer's list, where they changed without us seeing it.
	void refreshFromExplorer(const TrayIconMessage& message);
	void setIdentity(const QString& id, const QString& title, const QString& exePath);
	void setCategory(Category::Enum category) { this->bCategory = category; }

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	[[nodiscard]] bool hasMenu() const { return true; }
	[[nodiscard]] qs::menu::QsMenuHandle* menu() const { return nullptr; }
	[[nodiscard]] bool onlyMenu() const { return false; }
	// NOLINTEND(readability-convert-member-functions-to-static)

	// TrayHost's bookkeeping (GetTickCount64 times).
	struct Tracking {
		ULONGLONG hookUpdatedAt = 0;
		ULONGLONG announcedAt = 0;
		bool seenByExplorer = false;
		bool seenThisSnapshot = false;
		int missedSnapshots = 0;
	} tracking;

	[[nodiscard]] QBindable<QString> bindableId() { return &this->bId; }
	[[nodiscard]] QBindable<QString> bindableTitle() { return &this->bTitle; }
	[[nodiscard]] QBindable<Status::Enum> bindableStatus() { return &this->bStatus; }
	[[nodiscard]] QBindable<Category::Enum> bindableCategory() { return &this->bCategory; }
	[[nodiscard]] QBindable<QString> bindableIcon() { return &this->bIcon; }
	[[nodiscard]] QBindable<QString> bindableTooltipTitle() { return &this->bTooltipTitle; }
	[[nodiscard]] QBindable<QString> bindableTooltipDescription() {
		return &this->bTooltipDescription;
	}

signals:
	void ready();
	// A click came while the callback message is unknown; it's held until it is.
	void callbackNeeded();

	void idChanged();
	void titleChanged();
	void statusChanged();
	void categoryChanged();
	void iconChanged();
	void tooltipTitleChanged();
	void tooltipDescriptionChanged();
	void hasMenuChanged();
	void onlyMenuChanged();

private:
	enum class Pending : quint8 {
		None,
		Activate,
		DoubleClick,
		SecondaryActivate,
		Display,
	};

	void allowForeground() const;
	void send(UINT event) const;
	void sendActivate(bool doubleClick);
	void sendSecondaryActivate();
	void sendDisplay();
	void hold(Pending action);
	void replayPending();
	void setTip(const QString& tip);
	void updateIconSource();

	HWND hwnd;
	UINT uid;
	QUuid guid;
	UINT callbackMessage = 0;
	UINT version = 0;
	QString exePath;
	QString tip;
	TrayIconImage image;
	// For telling a second left click apart, like explorer does.
	ULONGLONG lastActivate = 0;
	Pending pending = Pending::None;
	ULONGLONG pendingAt = 0;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(SystemTrayItem, QString, bId, &SystemTrayItem::idChanged);
	Q_OBJECT_BINDABLE_PROPERTY(SystemTrayItem, QString, bTitle, &SystemTrayItem::titleChanged);
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(SystemTrayItem, Status::Enum, bStatus, Status::Active, &SystemTrayItem::statusChanged);
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(SystemTrayItem, Category::Enum, bCategory, Category::ApplicationStatus, &SystemTrayItem::categoryChanged);
	Q_OBJECT_BINDABLE_PROPERTY(SystemTrayItem, QString, bIcon, &SystemTrayItem::iconChanged);
	Q_OBJECT_BINDABLE_PROPERTY(SystemTrayItem, QString, bTooltipTitle, &SystemTrayItem::tooltipTitleChanged);
	Q_OBJECT_BINDABLE_PROPERTY(SystemTrayItem, QString, bTooltipDescription, &SystemTrayItem::tooltipDescriptionChanged);
	// clang-format on
};

} // namespace qs::windows::services::systray
