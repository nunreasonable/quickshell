#pragma once

#include <qbytearray.h>
#include <qbytearrayview.h>
#include <qcontainerfwd.h>
#include <qhash.h>
#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qscreen.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../../core/model.hpp"
#include "../../core/qmlscreen.hpp"
#include "../virtual_desktops.hpp"
#include "../window_tracker.hpp"

namespace qs::hyprland::ipc {

class HyprlandMonitor;
class HyprlandWorkspace;
class HyprlandToplevel;
class Dispatcher;

} // namespace qs::hyprland::ipc

Q_DECLARE_OPAQUE_POINTER(qs::hyprland::ipc::HyprlandWorkspace*);
Q_DECLARE_OPAQUE_POINTER(qs::hyprland::ipc::HyprlandMonitor*);
Q_DECLARE_OPAQUE_POINTER(qs::hyprland::ipc::HyprlandToplevel*);

namespace qs::hyprland::ipc {

///! Live Hyprland style event.
/// Event in the format Hyprland's event socket uses, generated from Windows window and virtual
/// desktop changes. Holding this object after the signal handler exits is undefined as the
/// event instance is reused.
///
/// Emitted by @@Hyprland.rawEvent(s).
class HyprlandIpcEvent: public QObject {
	Q_OBJECT;
	/// The name of the event.
	///
	/// See [Hyprland Wiki: IPC](https://wiki.hyprland.org/IPC/) for a list of events.
	Q_PROPERTY(QString name READ nameStr CONSTANT);
	/// The unparsed data of the event.
	Q_PROPERTY(QString data READ dataStr CONSTANT);
	QML_NAMED_ELEMENT(HyprlandEvent);
	QML_UNCREATABLE("HyprlandIpcEvents cannot be created.");

public:
	HyprlandIpcEvent(QObject* parent): QObject(parent) {}

	/// Parse this event with a known number of arguments.
	///
	/// Argument count is required as some events can contain commas
	/// in the last argument, which can be ignored as long as the count is known.
	Q_INVOKABLE [[nodiscard]] QVector<QString> parse(qint32 argumentCount) const;
	[[nodiscard]] QVector<QByteArrayView> parseView(qint32 argumentCount) const;

	[[nodiscard]] QString nameStr() const;
	[[nodiscard]] QString dataStr() const;

	QByteArray name;
	QByteArray data;
};

// The Windows "connection": the window tracker and virtual desktops presented the way the
// Hyprland IPC module does. Workspace N is virtual desktop N-1; monitors are the screens.
class HyprlandIpc: public QObject {
	Q_OBJECT;

public:
	static HyprlandIpc* instance();

	[[nodiscard]] QString requestSocketPath() const { return {}; }
	[[nodiscard]] QString eventSocketPath() const { return {}; }

	void dispatch(const QString& request);

	[[nodiscard]] HyprlandMonitor* monitorFor(QuickshellScreenInfo* screen);
	[[nodiscard]] HyprlandMonitor* monitorForScreen(QScreen* screen) const;

	[[nodiscard]] QBindable<HyprlandMonitor*> bindableFocusedMonitor() const {
		return &this->bFocusedMonitor;
	}

	[[nodiscard]] QBindable<HyprlandWorkspace*> bindableFocusedWorkspace() const {
		return &this->bFocusedWorkspace;
	}

	[[nodiscard]] QBindable<HyprlandToplevel*> bindableActiveToplevel() const {
		return &this->bActiveToplevel;
	}

	// Bumped whenever the monitor or workspace lists change, so bindings that look objects up
	// by index re-evaluate.
	[[nodiscard]] QBindable<quint32> bindableMonitorsVersion() const { return &this->bMonitorsVersion; }
	[[nodiscard]] QBindable<quint32> bindableWorkspacesVersion() const { return &this->bWorkspacesVersion; }

	[[nodiscard]] ObjectModel<HyprlandMonitor>* monitors();
	[[nodiscard]] ObjectModel<HyprlandWorkspace>* workspaces();
	[[nodiscard]] ObjectModel<HyprlandToplevel>* toplevels();

	[[nodiscard]] HyprlandWorkspace* workspaceById(qint32 id) const;
	[[nodiscard]] HyprlandWorkspace* workspaceByName(const QString& name) const;
	[[nodiscard]] HyprlandToplevel* findToplevelByAddress(quint64 address) const;
	[[nodiscard]] HyprlandToplevel* toplevelForWindow(qs::windows::TrackedWindow* window) const;

	[[nodiscard]] qs::windows::WindowTracker* tracker() const { return this->mTracker; }
	[[nodiscard]] qs::windows::VirtualDesktops* desktops() const { return this->mDesktops; }

	// Rebuild the lastIpcObject of every object of a kind. Everything is kept live on Windows,
	// so these only exist for API compatibility.
	void refreshWorkspaces();
	void refreshMonitors();
	void refreshToplevels();

	void emitEvent(const QByteArray& name, const QByteArray& data);

	// The last argument may contain commas, so the count is required.
	[[nodiscard]] static QVector<QByteArrayView> parseEventArgs(QByteArrayView event, quint16 count);

signals:
	void connected();
	void rawEvent(HyprlandIpcEvent* event);

	void focusedMonitorChanged();
	void focusedWorkspaceChanged();
	void activeToplevelChanged();

	/// Windows only: a `global <name>` dispatch (Hyprland's shortcut forwarding), with the name
	/// as given, e.g. `quickshell:overviewToggle`. The GlobalShortcut implementation connects here.
	void dispatchGlobal(const QString& name);

	// For attached HyprlandToplevels created before their window was known here.
	void toplevelAdded(HyprlandToplevel* toplevel);

private slots:
	void onWindowAdded(qs::windows::TrackedWindow* window);
	void onWindowRemoved(qs::windows::TrackedWindow* window);
	void onActiveWindowChanged();
	void onTrackerFlushed();
	void onDesktopsChanged();
	void onCurrentDesktopChanged();
	void onScreensChanged();
	void emitGeometryEvent();

private:
	explicit HyprlandIpc();
	Q_DISABLE_COPY_MOVE(HyprlandIpc);

	void syncMonitors(bool initial);
	void syncWorkspaces(bool initial);
	void updateFocusedMonitor(HyprlandMonitor* monitor);
	void updateFocusedWorkspace();

	qs::windows::WindowTracker* mTracker = nullptr;
	qs::windows::VirtualDesktops* mDesktops = nullptr;
	Dispatcher* dispatcher = nullptr;

	ObjectModel<HyprlandMonitor> mMonitors {this};
	ObjectModel<HyprlandWorkspace> mWorkspaces {this};
	ObjectModel<HyprlandToplevel> mToplevels {this};
	QHash<qs::windows::TrackedWindow*, HyprlandToplevel*> byWindow;

	HyprlandIpcEvent event {this};
	QTimer geometryTimer;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandIpc, HyprlandMonitor*, bFocusedMonitor, &HyprlandIpc::focusedMonitorChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandIpc, HyprlandWorkspace*, bFocusedWorkspace, &HyprlandIpc::focusedWorkspaceChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandIpc, HyprlandToplevel*, bActiveToplevel, &HyprlandIpc::activeToplevelChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandIpc, quint32, bMonitorsVersion);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandIpc, quint32, bWorkspacesVersion);
	// clang-format on
};

} // namespace qs::hyprland::ipc
