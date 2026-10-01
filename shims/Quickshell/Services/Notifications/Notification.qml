// Windows shim for Quickshell.Services.Notifications' Notification.
// Real backend: whatever the eventual Windows notification broker exposes
// (likely a WinRT ToastNotification wrapper, or a custom freedesktop-spec
// listener bridged to Windows' own notification center).
//
// Upstream is QML_UNCREATABLE and only ever handed out via
// NotificationServer.notification(s); since the shim's NotificationServer
// never actually receives/emits notifications yet, no instance of this type
// exists at runtime today, but it's registered for API completeness.
import QtQml

QtObject {
    id: root

    property int id: 0
    property bool tracked: false
    readonly property bool lastGeneration: false
    property real expireTimeout: -1
    property string appName: ""
    property string appIcon: ""
    property string summary: ""
    property string body: ""
    property int urgency: NotificationUrgency.Normal
    property var actions: []
    property bool hasActionIcons: false
    property bool resident: false
    // NOTE: upstream has a `transient` property here, but `transient` is a
    // reserved QML keyword and cannot be used as a property identifier in
    // hand-authored QML. ii never reads Notification.transient directly (it
    // only reads the unrelated `hints.transient` map entry), so this is
    // dropped rather than worked around.
    property string desktopEntry: ""
    property string image: ""
    property bool hasInlineReply: false
    property string inlineReplyPlaceholder: ""
    property var hints: ({})

    signal closed(int reason)

    function expire() {
        root.tracked = false;
        root.closed(NotificationCloseReason.Expired);
    }

    function dismiss() {
        root.tracked = false;
        root.closed(NotificationCloseReason.Dismissed);
    }

    function sendInlineReply(replyText) {
        console.info("[shim] Notification.sendInlineReply", replyText);
    }
}
