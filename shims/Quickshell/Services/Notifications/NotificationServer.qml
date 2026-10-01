// Windows shim for Quickshell.Services.Notifications' NotificationServer.
// Real backend: a Windows notification broker bridging into (or replacing)
// the freedesktop notification spec this type implements on Linux -- likely
// a WinRT ToastNotification listener, or hosting Windows' own notification
// pipe. ii instantiates this type directly (unlike most other shimmed
// types), so it's a plain creatable QtObject here too.
//
// No notifications are ever received yet, so `notification(s)` never fires
// and `trackedNotifications` stays empty; the support-flag properties below
// are all real read/write properties so ii's configuration of them doesn't
// error, they just have no effect until a backend exists.
import QtQml
import "../../../_common" as Common

QtObject {
    property bool keepOnReload: true
    property bool persistenceSupported: false
    property bool bodySupported: true
    property bool bodyMarkupSupported: false
    property bool bodyHyperlinksSupported: false
    property bool bodyImagesSupported: false
    property bool actionsSupported: false
    property bool actionIconsSupported: false
    property bool imageSupported: false
    property bool inlineReplySupported: false
    property var extraHints: []

    readonly property Common.ObjectModel trackedNotifications: Common.ObjectModel {}

    signal notification(QtObject notification)
}
