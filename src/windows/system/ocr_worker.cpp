#include "ocr_worker.hpp"

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qstring.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/base.h>

#include "ocr.hpp"

using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage;

namespace qs::windows::sys {

namespace {
Q_LOGGING_CATEGORY(logOcrWorker, "quickshell.windows.ocr", QtWarningMsg);

QString hresultError(const winrt::hresult_error& e) {
	return QStringLiteral("0x%1").arg(static_cast<uint32_t>(e.code().value), 8, 16, QLatin1Char('0'));
}
} // namespace

OcrWorker::OcrWorker(Ocr* frontend): mFrontend(frontend) {}

OcrWorker::~OcrWorker() = default;

void OcrWorker::start() {
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		this->mApartmentInitialized = true;
	} catch (const winrt::hresult_error& e) {
		qCWarning(logOcrWorker) << "init_apartment(multi_threaded) failed:" << hresultError(e);
	}
}

void OcrWorker::shutdown() {
	this->mEngine = nullptr;

	if (this->mApartmentInitialized) {
		winrt::uninit_apartment();
		this->mApartmentInitialized = false;
	}
}

winrt::Windows::Media::Ocr::OcrEngine OcrWorker::engine() {
	if (this->mEngine) return this->mEngine;

	try {
		this->mEngine = OcrEngine::TryCreateFromUserProfileLanguages();
	} catch (const winrt::hresult_error& e) {
		qCDebug(logOcrWorker) << "TryCreateFromUserProfileLanguages failed:" << hresultError(e);
	}

	if (!this->mEngine) {
		// No recognizer for any of the user's profile languages (or the OCR optional feature
		// isn't installed for them); fall back to whatever recognizer language IS installed
		// rather than failing outright - matches the Linux side's tesseract invocation, which
		// also just asks for every language it has (`tesseract --list-langs`).
		try {
			auto available = OcrEngine::AvailableRecognizerLanguages();
			if (available.Size() > 0) {
				this->mEngine = OcrEngine::TryCreateFromLanguage(available.GetAt(0));
			}
		} catch (const winrt::hresult_error& e) {
			qCWarning(logOcrWorker) << "AvailableRecognizerLanguages failed:" << hresultError(e);
		}
	}

	return this->mEngine;
}

void OcrWorker::cmdRecognize(int requestId, const QString& path) {
	QString resultText;
	auto ok = false;
	QString error;

	try {
		auto ocrEngine = this->engine();
		if (!ocrEngine) {
			error = QStringLiteral(
			    "No OCR language installed (Settings > Time & language > Language & region > "
			    "add a language, then install its handwriting/OCR optional feature)"
			);
		} else {
			auto wpath = path.toStdWString();
			auto file = StorageFile::GetFileFromPathAsync(winrt::hstring(wpath)).get();
			auto stream = file.OpenAsync(FileAccessMode::Read).get();
			auto decoder = BitmapDecoder::CreateAsync(stream).get();
			auto bitmap =
			    decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied)
			        .get();

			auto result = ocrEngine.RecognizeAsync(bitmap).get();
			resultText = QString::fromWCharArray(result.Text().c_str());
			ok = true;
		}
	} catch (const winrt::hresult_error& e) {
		error = QStringLiteral("OCR failed (%1)").arg(hresultError(e));
		qCWarning(logOcrWorker) << "Recognition failed for" << path << ":" << error;
	}

	QMetaObject::invokeMethod(
	    this->mFrontend,
	    [frontend = this->mFrontend, requestId, resultText, ok, error] {
		    frontend->backendDone(requestId, resultText, ok, error);
	    },
	    Qt::QueuedConnection
	);
}

} // namespace qs::windows::sys
