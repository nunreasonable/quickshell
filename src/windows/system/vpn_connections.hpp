#pragma once

#include <qlist.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qvariant.h>

namespace qs::windows::sys {

struct VpnEntry {
	QString name;
	bool connected = false;
};

QList<VpnEntry> listVpnConnections();

class VpnConnections: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit VpnConnections(QObject* parent = nullptr): QObject(parent) {}

	Q_INVOKABLE QVariantList list() const;
};

} // namespace qs::windows::sys
