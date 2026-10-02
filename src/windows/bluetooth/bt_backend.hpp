#pragma once

#include <memory>

#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtypes.h>

namespace qs::bluetooth {

class WinBluetooth;
class BtWorker;

///! GUI-thread owner of the Bluetooth worker thread.
/// Starts a dedicated MTA thread running a BtWorker (Qt's GUI thread is STA and can't block on
/// WinRT, see docs/AGENTS.md) and queues commands to it. Every method returns immediately; the
/// results come back to WinBluetooth as queued backend* calls.
class BtBackend {
public:
	explicit BtBackend(WinBluetooth* frontend);
	~BtBackend();
	Q_DISABLE_COPY_MOVE(BtBackend);

	void setPowered(bool on, quint64 seq);
	void setDiscovering(bool on);
	void pair(const QString& key);
	void cancelPair(const QString& key);
	void forget(const QString& key);
	void connectDevice(const QString& key, bool connect);

	/// Stops the worker thread (bounded wait). Safe to call more than once.
	void stop();

private:
	std::unique_ptr<QThread> mThread;
	BtWorker* mWorker = nullptr;
};

} // namespace qs::bluetooth
