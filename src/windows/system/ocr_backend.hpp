#pragma once

#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class Ocr;
class OcrWorker;

///! GUI-thread owner of the OCR worker thread. Starts a dedicated MTA thread
/// (`winrt::init_apartment(multi_threaded)`; Qt's GUI thread is STA, see docs/AGENTS.md)
/// running an OcrWorker, and relays recognition requests to it. The worker posts the result
/// back to `frontend` (an Ocr, GUI thread) via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.
/// Same split as GsmtcBackend/GsmtcWorker (Mpris).
class OcrBackend: public QObject {
	Q_OBJECT;

public:
	explicit OcrBackend(Ocr* frontend);
	~OcrBackend() override;
	Q_DISABLE_COPY_MOVE(OcrBackend);

	// Posts a command to the worker thread and returns immediately; the result arrives later
	// via Ocr::backendDone.
	void requestRecognize(int requestId, const QString& path);

private:
	QThread mThread;
	OcrWorker* mWorker = nullptr; // lives in mThread; created/destroyed around its lifetime
};

} // namespace qs::windows::sys
