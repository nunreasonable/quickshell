#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qpoint.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

enum class TaskbarEdge : quint8 { Left, Top, Right, Bottom };

class TaskbarManager: public QObject {
	Q_OBJECT;

public:
	static TaskbarManager* instance();
	~TaskbarManager() override;
	Q_DISABLE_COPY_MOVE(TaskbarManager);

	[[nodiscard]] bool hoverOnly() const { return this->mHoverOnly; }
	void setHoverOnly(bool hoverOnly);

	static void restoreForCrash();

signals:
	void hoverOnlyChanged();

private:
	explicit TaskbarManager(QObject* parent);

	struct Bar {
		HWND hwnd = nullptr;
		QRect monitor;
		TaskbarEdge edge = TaskbarEdge::Bottom;
		int thickness = 0;
	};

	void enable();
	void disable();
	void findBars();
	void onCursorMoved(QPoint position);
	void onCheck();
	void reveal();
	void conceal();
	[[nodiscard]] bool atTrigger(QPoint position) const;
	[[nodiscard]] bool overBar(QPoint position) const;
	[[nodiscard]] static bool taskbarPopupActive();

	bool mHoverOnly = false;
	bool enabled = false;
	bool revealed = false;
	qint64 leftAt = 0;
	QList<Bar> bars;
	QTimer checkTimer;
};

class Taskbar: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(bool hoverOnly READ hoverOnly WRITE setHoverOnly NOTIFY hoverOnlyChanged);
	// clang-format on

public:
	explicit Taskbar(QObject* parent = nullptr);

	[[nodiscard]] bool hoverOnly() const;
	void setHoverOnly(bool hoverOnly);

signals:
	void hoverOnlyChanged();
};

} // namespace qs::windows
