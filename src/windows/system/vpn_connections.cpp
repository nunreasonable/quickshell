#include "vpn_connections.hpp"

#include <vector>

#include <qset.h>
#include <windows.h>
#include <ras.h>
#include <raserror.h>

namespace qs::windows::sys {

namespace {

std::vector<RASENTRYNAMEW> phonebookEntries() {
	std::vector<RASENTRYNAMEW> list(1);
	list[0].dwSize = sizeof(RASENTRYNAMEW);
	DWORD bytes = sizeof(RASENTRYNAMEW);
	DWORD count = 0;
	auto result = RasEnumEntriesW(nullptr, nullptr, list.data(), &bytes, &count);
	if (result == ERROR_BUFFER_TOO_SMALL) {
		list.assign(bytes / sizeof(RASENTRYNAMEW) + 1, {});
		list[0].dwSize = sizeof(RASENTRYNAMEW);
		result = RasEnumEntriesW(nullptr, nullptr, list.data(), &bytes, &count);
	}
	if (result != ERROR_SUCCESS) return {};
	list.resize(count);
	return list;
}

bool isVpn(const RASENTRYNAMEW& entry) {
	DWORD bytes = 0;
	RasGetEntryPropertiesW(entry.szPhonebookPath, entry.szEntryName, nullptr, &bytes, nullptr, nullptr);
	if (bytes < sizeof(RASENTRYW)) return false;
	std::vector<char> buffer(bytes);
	auto* props = reinterpret_cast<RASENTRYW*>(buffer.data()); // NOLINT
	props->dwSize = sizeof(RASENTRYW);
	if (RasGetEntryPropertiesW(entry.szPhonebookPath, entry.szEntryName, props, &bytes, nullptr, nullptr) != ERROR_SUCCESS) {
		return false;
	}
	return props->dwType == RASET_Vpn;
}

QSet<QString> connectedNames() {
	std::vector<RASCONNW> list(1);
	list[0].dwSize = sizeof(RASCONNW);
	DWORD bytes = sizeof(RASCONNW);
	DWORD count = 0;
	auto result = RasEnumConnectionsW(list.data(), &bytes, &count);
	if (result == ERROR_BUFFER_TOO_SMALL) {
		list.assign(bytes / sizeof(RASCONNW) + 1, {});
		list[0].dwSize = sizeof(RASCONNW);
		result = RasEnumConnectionsW(list.data(), &bytes, &count);
	}
	QSet<QString> names;
	if (result != ERROR_SUCCESS) return names;
	for (DWORD i = 0; i < count; i++) {
		RASCONNSTATUSW status {};
		status.dwSize = sizeof(RASCONNSTATUSW);
		if (RasGetConnectStatusW(list[i].hrasconn, &status) == ERROR_SUCCESS
		    && status.rasconnstate == RASCS_Connected)
		{
			names.insert(QString::fromWCharArray(list[i].szEntryName));
		}
	}
	return names;
}

} // namespace

QList<VpnEntry> listVpnConnections() {
	QList<VpnEntry> out;
	auto connected = connectedNames();
	for (const auto& entry: phonebookEntries()) {
		if ((entry.dwFlags & REN_AllUsers) != 0 || !isVpn(entry)) continue;
		auto name = QString::fromWCharArray(entry.szEntryName);
		out.append({.name = name, .connected = connected.contains(name)});
	}
	return out;
}

QVariantList VpnConnections::list() const {
	QVariantList out;
	for (const auto& entry: listVpnConnections()) {
		out.append(QVariantMap {
		    {"Name", entry.name},
		    {"ConnectionStatus", entry.connected ? "Connected" : "Disconnected"},
		});
	}
	return out;
}

} // namespace qs::windows::sys
