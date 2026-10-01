// Windows shim for Quickshell.Services.Notifications' NotificationAction.
// Real backend: whatever the eventual Windows notification broker exposes
// for toast action buttons.
//
// Upstream is QML_UNCREATABLE and only ever handed out via Notification.actions;
// since no notifications are ever generated in this phase, no instance of
// this type exists at runtime today, but it's registered for API
// completeness and so Notification.qml can type its `actions` list.
import QtQml

QtObject {
    property string identifier: ""
    property string text: ""

    function invoke() {
        console.info("[shim] NotificationAction.invoke", identifier, text);
    }
}
