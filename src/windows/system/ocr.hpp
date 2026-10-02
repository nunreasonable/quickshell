#pragma once

#include <memory>

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class OcrBackend;

///! Text recognition for screenshots (replaces tesseract in the region selector), backed by
/// Windows.Media.Ocr (C++/WinRT). Recognition runs on a dedicated MTA worker thread owned by
/// OcrBackend/OcrWorker - see gsmtc_worker.hpp (Mpris) for the threading pattern this follows -
/// so the GUI thread never blocks on it. Call @@recognizeText and wait for @@recognized.
class Ocr: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Ocr(QObject* parent = nullptr);
	~Ocr() override;
	Q_DISABLE_COPY_MOVE(Ocr);

	/// Starts recognizing text in the image file at `path` and returns a request id at once;
	/// the result arrives later via @@recognized carrying the same id. The engine is picked
	/// from the user's profile languages, falling back to any installed recognizer language.
	Q_INVOKABLE int recognizeText(const QString& path);

	// Called by OcrWorker (through OcrBackend's queued post), GUI thread only.
	void backendDone(int requestId, const QString& text, bool ok, const QString& error);

signals:
	/// `ok` is false if the image couldn't be decoded or no OCR language is available; `error`
	/// then holds a short reason and `text` is empty.
	void recognized(int requestId, const QString& text, bool ok, const QString& error);

private:
	std::unique_ptr<OcrBackend> mBackend;
	int mNextRequestId = 1;
};

} // namespace qs::windows::sys
