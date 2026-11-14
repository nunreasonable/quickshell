#pragma once

#include <qt_windows.h>

#include <qcolor.h>
#include <qhash.h>
#include <qlist.h>
#include <qset.h>
#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

class TrackedWindow;
class WindowTracker;

class DecorationManager: public QObject {
	Q_OBJECT;

public:
	static DecorationManager* instance();
	~DecorationManager() override;
	Q_DISABLE_COPY_MOVE(DecorationManager);

	[[nodiscard]] static bool supported();
	static void restoreForCrash();

	[[nodiscard]] QString borders() const { return this->mBorders; }
	void setBorders(const QString& borders);
	[[nodiscard]] QColor activeColor() const { return this->mActiveColor; }
	void setActiveColor(const QColor& color);
	[[nodiscard]] QColor inactiveColor() const { return this->mInactiveColor; }
	void setInactiveColor(const QColor& color);
	[[nodiscard]] QString corners() const { return this->mCorners; }
	void setCorners(const QString& corners);
	[[nodiscard]] qreal activeOpacity() const { return this->mActiveOpacity; }
	void setActiveOpacity(qreal opacity);
	[[nodiscard]] qreal inactiveOpacity() const { return this->mInactiveOpacity; }
	void setInactiveOpacity(qreal opacity);

signals:
	void bordersChanged();
	void activeColorChanged();
	void inactiveColorChanged();
	void cornersChanged();
	void activeOpacityChanged();
	void inactiveOpacityChanged();

private:
	explicit DecorationManager(QObject* parent);

	enum class BorderMode : quint8 { System, Colors, None };

	struct Applied {
		bool border = false;
		bool corner = false;
		bool opacity = false;
		bool addedLayered = false;
		COLORREF color = 0;
		DWORD preference = 0;
		BYTE alpha = 255;
	};

	[[nodiscard]] BorderMode borderMode() const;
	[[nodiscard]] DWORD cornerPreference() const;
	[[nodiscard]] bool overriding() const;
	[[nodiscard]] bool opacityOverriding() const;
	[[nodiscard]] BYTE alphaFor(bool active) const;

	void update();
	void start();
	void stop();
	void applyAll();
	void apply(TrackedWindow* window);
	void watch(TrackedWindow* window);
	void forget(TrackedWindow* window);
	static void restore(HWND hwnd, const Applied& state);
	static void restoreOpacity(HWND hwnd, Applied& state);
	void restoreAll();
	void onActiveWindowChanged();

	WindowTracker* tracker = nullptr;
	QList<QMetaObject::Connection> connections;
	QHash<HWND, Applied> applied;
	QSet<TrackedWindow*> watched;
	HWND lastActive = nullptr;
	bool running = false;

	QString mBorders = QStringLiteral("system");
	QColor mActiveColor;
	QColor mInactiveColor;
	QString mCorners = QStringLiteral("default");
	qreal mActiveOpacity = 1;
	qreal mInactiveOpacity = 1;
};

} // namespace qs::windows
