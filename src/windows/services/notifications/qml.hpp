#pragma once

#include <QtCore/qtmetamacros.h>
#include <qcontainerfwd.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>

#include "../../../core/doc.hpp"
#include "../../../core/model.hpp"
#include "../../../core/reload.hpp"
#include "notification.hpp"
#include "server.hpp"

namespace qs::windows::services::notifications {

///! Desktop Notifications Server.
/// The Windows stand-in for the [Desktop Notifications Specification] server of the Linux
/// build, with the same API. Windows has no protocol for a shell to receive or replace other
/// apps' toasts, so notifications come from two places instead:
/// - the shell itself, through @@notifySend() (what `notify-send` is on Linux);
/// - the Windows notification center, mirrored while @@mirrorSystemNotifications is true:
///   every toast another app shows becomes a Notification here, closes when it leaves the
///   notification center, and dismissing it here removes it there.
///
/// Windows keeps showing its own toast banners next to the shell's popups; there is no API to
/// suppress them. Users who only want the shell's popups can turn "Show notification banners"
/// off per app (or turn on Do not disturb) in Settings > System > Notifications, see
/// `Quickshell.Windows.NotificationSettings.openSettings()`. The mirror keeps working then,
/// since the toasts still reach the notification center.
///
/// The capability properties below only matter on Linux, where they are advertised to D-Bus
/// clients; they are kept so configurations work unchanged.
///
/// [Desktop Notifications Specification]: https://specifications.freedesktop.org/notification-spec/notification-spec-latest.html
class NotificationServerQml: public PostReloadHook {
	Q_OBJECT;
	// clang-format off
	/// If notifications should be re-emitted when quickshell reloads. Defaults to true.
	///
	/// The @@Notification.lastGeneration flag will be
	/// set on notifications from the prior generation for further filtering/handling.
	Q_PROPERTY(bool keepOnReload READ keepOnReload WRITE setKeepOnReload NOTIFY keepOnReloadChanged);
	/// If the notification server should advertise that it can persist notifications in the background
	/// after going offscreen. Defaults to false.
	Q_PROPERTY(bool persistenceSupported READ persistenceSupported WRITE setPersistenceSupported NOTIFY persistenceSupportedChanged);
	/// If notification body text should be advertised as supported by the notification server.
	/// Defaults to true.
	///
	/// Note that returned notifications are likely to return body text even if this property is false,
	/// as it is only a hint.
	Q_PROPERTY(bool bodySupported READ bodySupported WRITE setBodySupported NOTIFY bodySupportedChanged);
	/// If notification body text should be advertised as supporting markup as described in [the specification]
	/// Defaults to false.
	///
	/// Note that returned notifications may still contain markup if this property is false,
	/// as it is only a hint. By default Text objects will try to render markup. To avoid this
	/// if any is sent, change @@QtQuick.Text.textFormat to `PlainText`.
	Q_PROPERTY(bool bodyMarkupSupported READ bodyMarkupSupported WRITE setBodyMarkupSupported NOTIFY bodyMarkupSupportedChanged);
	/// If notification body text should be advertised as supporting hyperlinks as described in [the specification]
	/// Defaults to false.
	///
	/// Note that returned notifications may still contain hyperlinks if this property is false, as it is only a hint.
	///
	/// [the specification]: https://specifications.freedesktop.org/notification-spec/notification-spec-latest.html#hyperlinks
	Q_PROPERTY(bool bodyHyperlinksSupported READ bodyHyperlinksSupported WRITE setBodyHyperlinksSupported NOTIFY bodyHyperlinksSupportedChanged);
	/// If notification body text should be advertised as supporting images as described in [the specification]
	/// Defaults to false.
	///
	/// Note that returned notifications may still contain images if this property is false, as it is only a hint.
	///
	/// [the specification]: https://specifications.freedesktop.org/notification-spec/notification-spec-latest.html#images
	Q_PROPERTY(bool bodyImagesSupported READ bodyImagesSupported WRITE setBodyImagesSupported NOTIFY bodyImagesSupportedChanged);
	/// If notification actions should be advertised as supported by the notification server. Defaults to false.
	Q_PROPERTY(bool actionsSupported READ actionsSupported WRITE setActionsSupported NOTIFY actionsSupportedChanged);
	/// If notification actions should be advertised as supporting the display of icons. Defaults to false.
	Q_PROPERTY(bool actionIconsSupported READ actionIconsSupported WRITE setActionIconsSupported NOTIFY actionIconsSupportedChanged);
	/// If the notification server should advertise that it supports images. Defaults to false.
	Q_PROPERTY(bool imageSupported READ imageSupported WRITE setImageSupported NOTIFY imageSupportedChanged);
	/// If the notification server should advertise that it supports inline replies. Defaults to false.
	Q_PROPERTY(bool inlineReplySupported READ inlineReplySupported WRITE setInlineReplySupported NOTIFY inlineReplySupportedChanged);
	/// All notifications currently tracked by the server.
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::windows::services::notifications::Notification>*);
	Q_PROPERTY(UntypedObjectModel* trackedNotifications READ trackedNotifications NOTIFY trackedNotificationsChanged);
	/// Extra hints to expose to notification clients.
	Q_PROPERTY(QVector<QString> extraHints READ extraHints WRITE setExtraHints NOTIFY extraHintsChanged);
	/// (Windows only) If toasts from other apps in the Windows notification center should be
	/// mirrored as notifications. Defaults to true. Toasts already there when mirroring starts
	/// aren't announced, only the ones that arrive after.
	///
	/// Reading the notification center needs notification access (Settings > Privacy & security
	/// > Notifications), which Windows grants by default; see @@systemNotificationAccess.
	Q_PROPERTY(bool mirrorSystemNotifications READ mirrorSystemNotifications WRITE setMirrorSystemNotifications NOTIFY mirrorSystemNotificationsChanged);
	/// (Windows only) If the shell may read the Windows notification center.
	Q_PROPERTY(qs::windows::services::notifications::SystemNotificationAccess::Enum systemNotificationAccess READ systemNotificationAccess NOTIFY systemNotificationAccessChanged);
	/// (Windows only) If Windows toasts are being mirrored right now: @@mirrorSystemNotifications
	/// is on and access is allowed.
	Q_PROPERTY(bool systemMirrorActive READ systemMirrorActive NOTIFY systemMirrorActiveChanged);
	// clang-format on
	QML_NAMED_ELEMENT(NotificationServer);

public:
	void onPostReload() override;

	[[nodiscard]] bool keepOnReload() const;
	void setKeepOnReload(bool keepOnReload);

	[[nodiscard]] bool persistenceSupported() const;
	void setPersistenceSupported(bool persistenceSupported);

	[[nodiscard]] bool bodySupported() const;
	void setBodySupported(bool bodySupported);

	[[nodiscard]] bool bodyMarkupSupported() const;
	void setBodyMarkupSupported(bool bodyMarkupSupported);

	[[nodiscard]] bool bodyHyperlinksSupported() const;
	void setBodyHyperlinksSupported(bool bodyHyperlinksSupported);

	[[nodiscard]] bool bodyImagesSupported() const;
	void setBodyImagesSupported(bool bodyImagesSupported);

	[[nodiscard]] bool actionsSupported() const;
	void setActionsSupported(bool actionsSupported);

	[[nodiscard]] bool actionIconsSupported() const;
	void setActionIconsSupported(bool actionIconsSupported);

	[[nodiscard]] bool imageSupported() const;
	void setImageSupported(bool imageSupported);

	[[nodiscard]] bool inlineReplySupported() const;
	void setInlineReplySupported(bool inlineReplySupported);

	[[nodiscard]] QVector<QString> extraHints() const;
	void setExtraHints(QVector<QString> extraHints);

	[[nodiscard]] ObjectModel<Notification>* trackedNotifications() const;

	[[nodiscard]] bool mirrorSystemNotifications() const;
	void setMirrorSystemNotifications(bool mirror);

	[[nodiscard]] static SystemNotificationAccess::Enum systemNotificationAccess();
	[[nodiscard]] static bool systemMirrorActive();

	/// (Windows only) Sends a notification from the shell itself, taking the same options as
	/// `notify-send` (everything but summary and body: `-a`/`--app-name`, `-u`/`--urgency`,
	/// `-i`/`--icon`, `-t`/`--expire-time`, `-c`/`--category`, `-e`/`--transient`,
	/// `-h`/`--hint TYPE:NAME:VALUE`, `-A`/`--action [NAME=]Text`, `-r`/`--replace-id`).
	/// Returns the id the notification will have. Like a D-Bus notification, it arrives through
	/// @@notification(s) on the next event loop turn, not during this call.
	///
	/// Invoking one of its actions emits @@actionInvoked(s).
	Q_INVOKABLE quint32 notifySend(const QString& summary, const QString& body, const QStringList& args = {});

	/// (Windows only) Asks Windows for notification access again (it only prompts while no
	/// decision is recorded) and restarts mirroring if it is granted.
	Q_INVOKABLE void requestSystemNotificationAccess();

signals:
	/// Sent when a notification is received by the server.
	///
	/// If this notification should not be discarded, set its `tracked` property to true.
	void notification(qs::windows::services::notifications::Notification* notification);
	/// (Windows only) Sent when an action of a notification is invoked; what the `ActionInvoked`
	/// D-Bus signal tells the sender on Linux.
	void actionInvoked(quint32 id, const QString& action);

	void keepOnReloadChanged();
	void persistenceSupportedChanged();
	void bodySupportedChanged();
	void bodyMarkupSupportedChanged();
	void bodyHyperlinksSupportedChanged();
	void bodyImagesSupportedChanged();
	void actionsSupportedChanged();
	void actionIconsSupportedChanged();
	void imageSupportedChanged();
	void inlineReplySupportedChanged();
	void extraHintsChanged();
	void trackedNotificationsChanged();
	void mirrorSystemNotificationsChanged();
	void systemNotificationAccessChanged();
	void systemMirrorActiveChanged();

private:
	void updateSupported();

	bool live = false;
	bool mKeepOnReload = true;
	bool mMirrorSystemNotifications = true;
	NotificationServerSupport support;
};

} // namespace qs::windows::services::notifications
