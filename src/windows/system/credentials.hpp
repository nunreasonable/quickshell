#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

///! Generic credential storage (`CredReadW`/`CredWriteW`/`CredDeleteW`), for KeyringStorage's
/// API-key blob (replaces `secret-tool`).
class Credentials: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Credentials(QObject* parent = nullptr): QObject(parent) {}

	/// Stores `secret` as a generic credential under `target`, persisted for this Windows user.
	Q_INVOKABLE static bool write(const QString& target, const QString& secret);
	/// Reads back whatever was last written to `target`, or an empty string if there's none.
	Q_INVOKABLE static QString read(const QString& target);
	Q_INVOKABLE static bool remove(const QString& target);
	Q_INVOKABLE static bool exists(const QString& target);
};

} // namespace qs::windows::sys
