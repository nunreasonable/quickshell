pragma Singleton
// Windows shim for the Quickshell.Hyprland "Hyprland" singleton.
// Real backend: a native Windows virtual-desktop + top-level-window tracker
// (IVirtualDesktopManager for workspaces, EnumWindows/shell hooks for
// toplevels) exposing this same dispatch/monitor/workspace/toplevel surface.
//
// Monitors are derived 1:1 from Quickshell.screens (cross-platform core, not
// part of this shim). Ten fake workspaces (id 1..10) are always present with
// workspace 1 active/focused on the first monitor, so bar/overview workspace
// indicators have something sane to render. `dispatch()` and the refresh*()
// functions are no-ops that just log what was asked for.
import QtQml
import Quickshell
import "../../_common" as Common

QtObject {
    id: root

    property var _workspaceList: []
    property var _monitorCache: ({})

    readonly property Common.ObjectModel workspaces: Common.ObjectModel { id: workspacesModel }
    readonly property Common.ObjectModel monitors: Common.ObjectModel { id: monitorsModel }
    readonly property Common.ObjectModel toplevels: Common.ObjectModel { id: toplevelsModel }

    property QtObject focusedMonitor: null
    property QtObject focusedWorkspace: null
    readonly property QtObject activeToplevel: null

    readonly property string requestSocketPath: ""
    readonly property string eventSocketPath: ""

    /// Never emitted in the shim (no real event socket); kept so
    /// `Hyprland.onRawEvent` handlers in ii parse and simply never fire.
    signal rawEvent(var event)

    property Component _workspaceComponent: Component { HyprlandWorkspace {} }
    property Component _monitorComponent: Component { HyprlandMonitor {} }

    /// Mirrors upstream's Q_INVOKABLE HyprlandMonitor* monitorFor(ShellScreen*).
    /// Returns a stable, cached HyprlandMonitor per Quickshell screen name.
    function monitorFor(screen) {
        if (!screen) return null;
        var existing = root._monitorCache[screen.name];
        if (existing) return existing;

        var idx = Object.keys(root._monitorCache).length;
        var mon = root._monitorComponent.createObject(root, {
            id: idx,
            name: screen.name,
            description: screen.model || "",
            x: screen.x,
            y: screen.y,
            width: screen.width,
            height: screen.height,
            scale: screen.devicePixelRatio,
            focused: idx === 0,
        });
        mon.activeWorkspace = root._workspaceList.length > 0 ? root._workspaceList[0] : null;

        root._monitorCache[screen.name] = mon;
        var values = monitorsModel.values;
        values.push(mon);
        monitorsModel.values = values;

        if (idx === 0) {
            root.focusedMonitor = mon;
            for (var i = 0; i < root._workspaceList.length; i++) {
                root._workspaceList[i].monitor = mon;
            }
        }

        return mon;
    }

    function _init() {
        var list = [];
        for (var i = 1; i <= 10; i++) {
            var ws = root._workspaceComponent.createObject(root, {
                id: i,
                name: String(i),
                active: i === 1,
                focused: i === 1,
            });
            list.push(ws);
        }
        root._workspaceList = list;
        workspacesModel.values = list;
        root.focusedWorkspace = list[0];

        var screens = Quickshell.screens;
        for (var s = 0; s < screens.length; s++) {
            root.monitorFor(screens[s]);
        }
    }

    property QtObject _screenWatcher: Connections {
        target: Quickshell
        function onScreensChanged() {
            var screens = Quickshell.screens;
            for (var s = 0; s < screens.length; s++) {
                root.monitorFor(screens[s]);
            }
        }
    }

    Component.onCompleted: root._init()

    function dispatch(request) {
        console.info("[shim] Hyprland.dispatch", request);
    }

    function refreshMonitors() {
        console.info("[shim] Hyprland.refreshMonitors (no-op)");
    }

    function refreshWorkspaces() {
        console.info("[shim] Hyprland.refreshWorkspaces (no-op)");
    }

    function refreshToplevels() {
        console.info("[shim] Hyprland.refreshToplevels (no-op)");
    }
}
