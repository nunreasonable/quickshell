#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qobject.h>
#include <qprocess.h>
#include <qtest.h>
#include <qtmetamacros.h>

#include "vpn_connections.hpp"

class VpnConnectionsTest: public QObject {
	Q_OBJECT;

private slots:
	static void matchesGetVpnConnection() {
		QProcess ps;
		ps.start(
		    "powershell",
		    {"-NoProfile",
		     "-Command",
		     "Get-VpnConnection -ErrorAction SilentlyContinue | Select-Object Name,ConnectionStatus | "
		     "ConvertTo-Json -Compress"}
		);
		QVERIFY(ps.waitForFinished(60000));
		auto doc = QJsonDocument::fromJson(ps.readAllStandardOutput());
		auto expected = doc.isArray() ? doc.array() : doc.isObject() ? QJsonArray {doc.object()} : QJsonArray {};

		auto native = qs::windows::sys::VpnConnections().list();
		qInfo() << "Get-VpnConnection:" << expected.size() << "native:" << native.size();
		QCOMPARE(native.size(), expected.size());
		for (qsizetype i = 0; i < native.size(); i++) {
			auto entry = native[i].toMap();
			auto want = expected[i].toObject();
			QCOMPARE(entry["Name"].toString(), want["Name"].toString());
			QCOMPARE(
			    entry["ConnectionStatus"].toString() == "Connected",
			    want["ConnectionStatus"].toString() == "Connected"
			);
		}
	}
};

QTEST_GUILESS_MAIN(VpnConnectionsTest)
#include "vpn_connections_test.moc"
