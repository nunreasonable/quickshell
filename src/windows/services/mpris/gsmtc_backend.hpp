#pragma once

#include <qobject.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtypes.h>

#include "player.hpp"

namespace qs::windows::services::mpris {

class Mpris;
class GsmtcWorker;

///! GUI-thread owner of the GSMTC worker thread.
/// Starts a dedicated MTA thread (`winrt::init_apartment(multi_threaded)`; Qt's GUI thread is
/// STA and can't host WinRT calls, see docs/AGENTS.md) running a GsmtcWorker, and relays
/// MprisPlayer control calls to it. The worker posts state back to `frontend` (an Mpris, GUI
/// thread) via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; never blocks the GUI
/// thread on a WinRT async operation.
class GsmtcBackend: public QObject {
	Q_OBJECT;

public:
	explicit GsmtcBackend(Mpris* frontend);
	~GsmtcBackend() override;
	Q_DISABLE_COPY_MOVE(GsmtcBackend);

	// Each of these posts a command to the worker thread and returns immediately; the resulting
	// property changes arrive later via Mpris::backendUpdate*, same as a real MPRIS player.
	void play(quint64 sessionId);
	void pause(quint64 sessionId);
	void togglePlayPause(quint64 sessionId);
	void next(quint64 sessionId);
	void previous(quint64 sessionId);
	void setPosition(quint64 sessionId, qint64 ticks);
	void setShuffle(quint64 sessionId, bool shuffle);
	void setLoopState(quint64 sessionId, MprisLoopState::Enum state);

private:
	QThread mThread;
	GsmtcWorker* mWorker = nullptr; // lives in mThread; created/destroyed around its lifetime
};

} // namespace qs::windows::services::mpris
