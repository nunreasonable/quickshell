#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>

#include "../../core/doc.hpp"
#include "../../core/model.hpp"
#include "../../core/qmlscreen.hpp"
#include "connection.hpp"
#include "monitor.hpp"

namespace qs::hyprland::ipc {

///! Hyprland API on Windows.
/// The Hyprland singleton backed by Windows: virtual desktops are workspaces (workspace N is
/// desktop N), screens are monitors, Alt+Tab windows are toplevels. @@dispatch() understands
/// the dispatchers configurations commonly use, in the classic (`workspace 2`) and the Lua
/// (`hl.dsp.focus({workspace = 2})`) spelling.
class HyprlandIpcQml: public QObject {
	Q_OBJECT;
	// clang-format off
	/// Always empty on Windows (no request socket).
	Q_PROPERTY(QString requestSocketPath READ requestSocketPath CONSTANT);
	/// Always empty on Windows (no event socket).
	Q_PROPERTY(QString eventSocketPath READ eventSocketPath CONSTANT);
	/// Always false on Windows: @@dispatch() takes both spellings, so the classic one keeps working.
	Q_PROPERTY(bool usingLua READ usingLua CONSTANT);
	/// The monitor with the foreground window. May be null.
	Q_PROPERTY(qs::hyprland::ipc::HyprlandMonitor* focusedMonitor READ default NOTIFY focusedMonitorChanged BINDABLE bindableFocusedMonitor);
	/// The current virtual desktop. May be null.
	Q_PROPERTY(qs::hyprland::ipc::HyprlandWorkspace* focusedWorkspace READ default NOTIFY focusedWorkspaceChanged BINDABLE bindableFocusedWorkspace);
	/// Foreground window (might be null)
	Q_PROPERTY(qs::hyprland::ipc::HyprlandToplevel* activeToplevel READ default NOTIFY activeToplevelChanged BINDABLE bindableActiveToplevel);
	/// All monitors (screens).
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::hyprland::ipc::HyprlandMonitor>*);
	Q_PROPERTY(UntypedObjectModel* monitors READ monitors CONSTANT);
	/// All workspaces (virtual desktops), sorted by id.
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::hyprland::ipc::HyprlandWorkspace>*);
	Q_PROPERTY(UntypedObjectModel* workspaces READ workspaces CONSTANT);
	/// All toplevels
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::hyprland::ipc::HyprlandToplevel>*);
	Q_PROPERTY(UntypedObjectModel* toplevels READ toplevels CONSTANT);
	// clang-format on
	QML_NAMED_ELEMENT(Hyprland);
	QML_SINGLETON;

public:
	explicit HyprlandIpcQml();

	/// Execute a hyprland [dispatcher](https://wiki.hyprland.org/Configuring/Dispatchers).
	/// Unknown dispatchers are logged once and ignored.
	Q_INVOKABLE static void dispatch(const QString& request);

	/// Get the HyprlandMonitor object that corrosponds to a quickshell screen.
	Q_INVOKABLE static HyprlandMonitor* monitorFor(QuickshellScreenInfo* screen);

	/// Refresh monitor information. Everything is live on Windows; rebuilds `lastIpcObject`s.
	Q_INVOKABLE static void refreshMonitors();

	/// Refresh workspace information. Everything is live on Windows; rebuilds `lastIpcObject`s.
	Q_INVOKABLE static void refreshWorkspaces();

	/// Refresh toplevel information. Everything is live on Windows; rebuilds `lastIpcObject`s.
	Q_INVOKABLE static void refreshToplevels();

	[[nodiscard]] static QString requestSocketPath();
	[[nodiscard]] static QString eventSocketPath();
	[[nodiscard]] static bool usingLua() { return false; }
	[[nodiscard]] static QBindable<HyprlandMonitor*> bindableFocusedMonitor();
	[[nodiscard]] static QBindable<HyprlandWorkspace*> bindableFocusedWorkspace();
	[[nodiscard]] static QBindable<HyprlandToplevel*> bindableActiveToplevel();
	[[nodiscard]] static ObjectModel<HyprlandMonitor>* monitors();
	[[nodiscard]] static ObjectModel<HyprlandWorkspace>* workspaces();
	[[nodiscard]] static ObjectModel<HyprlandToplevel>* toplevels();

signals:
	/// Emitted for every event the Windows backend generates, named like Hyprland's:
	/// `workspace`, `workspacev2`, `focusedmon`, `activewindow`, `activewindowv2`, `openwindow`,
	/// `closewindow`, `movewindow`, `movewindowv2`, `windowtitle`, `windowtitlev2`, `fullscreen`,
	/// `createworkspace(v2)`, `destroyworkspace(v2)`, `renameworkspace`, `monitoradded(v2)`,
	/// `monitorremoved`, plus `windowgeometry` (no Hyprland equivalent) when windows move or resize.
	///
	/// See [Hyprland Wiki: IPC](https://wiki.hyprland.org/IPC/) for a list of events.
	void rawEvent(qs::hyprland::ipc::HyprlandIpcEvent* event);

	void focusedMonitorChanged();
	void focusedWorkspaceChanged();
	void activeToplevelChanged();
};

} // namespace qs::hyprland::ipc
