#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <quuid.h>

#include "hook.hpp"
#include "item.hpp"

namespace qs::windows::services::systray {

// Process wide list of tray icons, fed by the hook and the explorer seed (on their threads,
// queued here). Started by the first SystemTray singleton, lives as long as the process.
class TrayHost: public QObject {
	Q_OBJECT;

public:
	static TrayHost* instance();

	[[nodiscard]] const QList<SystemTrayItem*>& items() const { return this->mItems; }

signals:
	void itemAdded(SystemTrayItem* item);
	void itemRemoved(SystemTrayItem* item);

private:
	explicit TrayHost();

	void apply(const TrayIconMessage& message);
	void add(const TrayIconMessage& message);
	void remove(SystemTrayItem* item);
	void pruneDead();
	[[nodiscard]] SystemTrayItem* find(HWND hwnd, UINT uid, const QUuid& guid) const;
	[[nodiscard]] QString uniqueId(const QString& base, UINT uid) const;
	[[nodiscard]] bool deletedRecently(const TrayIconMessage& message);

	struct Deletion {
		HWND hwnd;
		UINT uid;
		QUuid guid;
		ULONGLONG at;
	};

	QList<SystemTrayItem*> mItems;
	// The seed reads explorer's list on another thread: what it reports may predate a delete.
	QList<Deletion> deletions;
	QTimer pruneTimer;
};

} // namespace qs::windows::services::systray
