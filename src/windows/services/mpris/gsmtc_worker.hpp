#pragma once

#include <memory>
#include <unordered_map>

#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qvector.h>

#include <winrt/Windows.Media.Control.h>

#include "player.hpp"

namespace qs::windows::services::mpris {

class Mpris;

///! Runs on a dedicated MTA thread; owns the GSMTC session manager and all per-session state.
/// Created with no parent and moved to GsmtcBackend's QThread before it starts; every method
/// (other than the constructor) must only run on that thread -- commands arrive via
/// `QMetaObject::invokeMethod(worker, ..., Qt::QueuedConnection)` and GSMTC's own event
/// callbacks (which WinRT may invoke on an arbitrary thread pool thread) immediately re-post
/// themselves onto this object's queue the same way before touching any state.
class GsmtcWorker: public QObject {
	Q_OBJECT;

public:
	explicit GsmtcWorker(Mpris* frontend);
	~GsmtcWorker() override;
	Q_DISABLE_COPY_MOVE(GsmtcWorker);

	// Commands, invoked (queued) from the GUI thread via GsmtcBackend.
	void cmdPlay(quint64 sessionId);
	void cmdPause(quint64 sessionId);
	void cmdTogglePlayPause(quint64 sessionId);
	void cmdNext(quint64 sessionId);
	void cmdPrevious(quint64 sessionId);
	void cmdSetPosition(quint64 sessionId, qint64 ticks);
	void cmdSetShuffle(quint64 sessionId, bool shuffle);
	void cmdSetLoopState(quint64 sessionId, MprisLoopState::Enum state);

public slots:
	// Connected to QThread::started/finished; run winrt::init_apartment/uninit_apartment and
	// hold the session manager for exactly the worker thread's lifetime.
	void start();
	void shutdown();

private:
	struct SessionEntry {
		winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session {nullptr};
		winrt::event_token mediaToken {};
		winrt::event_token playbackToken {};
		winrt::event_token timelineToken {};
		QString trackKey; // dedup key (title+artist+album) used to notice track changes
		quint32 uniqueId = 0;
		QString cachedArtUrl; // reused while trackKey doesn't change ("write once per track")
	};

	void resyncSessions();
	void addSession(const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session);
	void removeSession(quint64 sessionId);
	[[nodiscard]] bool sessionStillTracked(
	    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session
	) const;

	void refreshPlaybackInfo(quint64 sessionId);
	void refreshTimeline(quint64 sessionId);
	void refreshMediaProperties(quint64 sessionId);

	[[nodiscard]] MprisPlayer::PlaybackSnapshot
	buildPlaybackSnapshot(const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session
	) const;
	[[nodiscard]] MprisPlayer::TimelineSnapshot buildTimelineSnapshot(
	    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session
	) const;

	Mpris* mFrontend; // GUI thread object; all posts to it are via invokeMethod(mFrontend, ...)

	winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager mManager {
	    nullptr
	};
	winrt::event_token mSessionsChangedToken {};
	winrt::event_token mCurrentSessionChangedToken {};

	std::unordered_map<quint64, std::unique_ptr<SessionEntry>> mEntries;
	quint64 mNextId = 1;
	bool mApartmentInitialized = false;
};

} // namespace qs::windows::services::mpris
