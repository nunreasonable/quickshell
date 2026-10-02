#pragma once

#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

#include <winrt/Windows.Media.Ocr.h>

namespace qs::windows::sys {

class Ocr;

///! Runs on a dedicated MTA thread; owns the OcrEngine for the process's lifetime. Created with
/// no parent and moved to OcrBackend's QThread before it starts; every method other than the
/// constructor must only run on that thread - commands arrive via
/// `QMetaObject::invokeMethod(worker, ..., Qt::QueuedConnection)`, same as GsmtcWorker (Mpris).
class OcrWorker: public QObject {
	Q_OBJECT;

public:
	explicit OcrWorker(Ocr* frontend);
	~OcrWorker() override;
	Q_DISABLE_COPY_MOVE(OcrWorker);

	// Command, invoked (queued) from the GUI thread via OcrBackend.
	void cmdRecognize(int requestId, const QString& path);

public slots:
	// Connected to QThread::started/finished: run winrt::init_apartment/uninit_apartment and
	// hold the engine for exactly the worker thread's lifetime.
	void start();
	void shutdown();

private:
	// Lazily creates (and caches) the recognizer engine: the user's profile languages first,
	// falling back to whatever recognizer language is installed. Returns a null engine (falsy)
	// if neither is available.
	[[nodiscard]] winrt::Windows::Media::Ocr::OcrEngine engine();

	Ocr* mFrontend; // GUI thread object; all posts to it are via invokeMethod(mFrontend, ...)

	bool mApartmentInitialized = false;
	winrt::Windows::Media::Ocr::OcrEngine mEngine {nullptr};
};

} // namespace qs::windows::sys
