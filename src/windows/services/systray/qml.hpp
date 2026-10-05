#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>

#include "../../../core/doc.hpp"
#include "../../../core/model.hpp"
#include "item.hpp"

namespace qs::windows::services::systray {

///! System tray
/// Windows backend: the icons apps put in the notification area with `Shell_NotifyIcon`.
/// Explorer keeps its own tray; see SystemTrayItem for how clicks and menus map.
class SystemTray: public QObject {
	Q_OBJECT;
	/// List of all system tray icons.
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::windows::services::systray::SystemTrayItem>*);
	Q_PROPERTY(UntypedObjectModel* items READ items CONSTANT);
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit SystemTray(QObject* parent = nullptr);

	[[nodiscard]] ObjectModel<SystemTrayItem>* items() { return &this->mItems; }

private:
	ObjectModel<SystemTrayItem> mItems {this};
};

} // namespace qs::windows::services::systray
