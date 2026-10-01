pragma Singleton
// Windows shim for the Quickshell.Services.SystemTray "SystemTray" singleton.
// Real backend: Shell_NotifyIcon / the Windows notification-area COM
// interfaces (ITrayNotify or polling the taskbar's notification area).
//
// `items` is always empty -- no tray-icon host is wired up yet.
import QtQml
import "../../../_common" as Common

QtObject {
    readonly property Common.ObjectModel items: Common.ObjectModel {}
}
