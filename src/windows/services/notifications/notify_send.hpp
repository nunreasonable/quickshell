#pragma once

#include <qcontainerfwd.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtypes.h>
#include <qvariant.h>

namespace qs::windows::services::notifications {

// What `notify-send <summary> <body> <args...>` would have put on the D-Bus Notify call.
struct NotifySendRequest {
	QString appName = QStringLiteral("notify-send");
	QString appIcon;
	quint32 replacesId = 0;
	QStringList actions; // identifier, text, identifier, text, ...
	QVariantMap hints;
	qint32 expireTimeout = -1;
};

// Parses libnotify's notify-send options (everything but the summary and body):
// -u/--urgency, -t/--expire-time, -a/--app-name, -i/--icon, -c/--category, -e/--transient,
// -h/--hint TYPE:NAME:VALUE, -A/--action [NAME=]Text, -r/--replace-id. -p/--print-id and
// -w/--wait are accepted and ignored. Values may be attached (`-ucritical`, `--urgency=critical`)
// or follow as the next argument, like GLib's option parser allows.
NotifySendRequest parseNotifySendArgs(const QStringList& args);

} // namespace qs::windows::services::notifications
