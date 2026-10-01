// Windows shim for Quickshell.Services.SystemTray's SystemTrayItem.
// Real backend: Shell_NotifyIcon / the Windows notification-area COM
// interfaces (iconography) plus whatever menu each tray app registers.
//
// Upstream is QML_UNCREATABLE and only ever handed out via SystemTray.items,
// which is always empty in this phase, so no instance exists at runtime
// today. `menu` is upstream a DBusMenuHandle* (an opaque handle from the
// core Quickshell.Widgets/menu machinery, out of this shim's scope); it's
// just `null` here since hasMenu is always false too.
import QtQml

QtObject {
    id: root

    property string id: ""
    property string title: ""
    property int status: Status.Passive
    property int category: Category.ApplicationStatus
    property string icon: ""
    property string tooltipTitle: ""
    property string tooltipDescription: ""
    property bool hasMenu: false
    property var menu: null
    property bool onlyMenu: false

    signal ready()

    function activate() {
        console.info("[shim] SystemTrayItem.activate", root.id);
    }

    function secondaryActivate() {
        console.info("[shim] SystemTrayItem.secondaryActivate", root.id);
    }

    function scroll(delta, horizontal) {
        console.info("[shim] SystemTrayItem.scroll", root.id, delta, horizontal);
    }

    function display(parentWindow, relativeX, relativeY) {
        console.info("[shim] SystemTrayItem.display", root.id, relativeX, relativeY);
    }
}
