#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qqmlparserstatus.h>
#include <qquickwindow.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

namespace qs::windows {

///! Click-outside / focus-loss dismissal for popups.
/// Windows counterpart of the hyprland_focus_grab_v1 grab behind HyprlandFocusGrab: while
/// active, a mouse button press outside every listed window (or their owned popups), or another
/// process taking the foreground, clears the grab and emits `cleared()`.
///
/// When the grab starts and the foreground is not one of the listed windows, the first visible
/// listed window that accepts focus is activated, like Hyprland focusing a grabbed surface.
///
/// Clicks come from the input mask tracker's low level mouse hook (no second hook), the
/// foreground from an EVENT_SYSTEM_FOREGROUND win event hook, both only while a grab is active.
class FocusGrab
    : public QObject
    , public QQmlParserStatus {
	Q_OBJECT;
	QML_ELEMENT;
	Q_INTERFACES(QQmlParserStatus);
	/// If the grab is active. It becomes true once at least one listed window is visible, and
	/// false when the grab is cleared.
	Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged);
	/// The windows (PanelWindow, FloatingWindow, PopupWindow...) that keep the grab alive.
	Q_PROPERTY(QList<QObject*> windows READ windows WRITE setWindows NOTIFY windowsChanged);

public:
	explicit FocusGrab(QObject* parent = nullptr): QObject(parent) {}
	~FocusGrab() override;
	Q_DISABLE_COPY_MOVE(FocusGrab);

	void classBegin() override {}
	void componentComplete() override;

	[[nodiscard]] bool isActive() const { return this->grabActive; }
	void setActive(bool active);

	[[nodiscard]] QObjectList windows() const { return this->windowObjects; }
	void setWindows(QObjectList windows);

	// Used by the tracker.
	[[nodiscard]] bool containsWindow(HWND hwnd) const;
	[[nodiscard]] DWORD activatedAt() const { return this->mActivatedAt; }
	void clear();

signals:
	/// The grab was cleared by a click outside or a focus change, or because none of its
	/// windows is visible anymore.
	void cleared();
	void activeChanged();
	void windowsChanged();

private slots:
	void onWindowsChanged();
	void onObjectDestroyed(QObject* object);

private:
	[[nodiscard]] QList<QQuickWindow*> visibleWindows() const;
	void tryActivate();
	void deactivate();
	void refocus();

	bool complete = false;
	bool targetActive = false;
	bool grabActive = false;
	DWORD mActivatedAt = 0;
	QObjectList windowObjects;
	QList<HWND> knownWindows;
};

} // namespace qs::windows
