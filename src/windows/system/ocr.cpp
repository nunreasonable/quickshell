#include "ocr.hpp"

#include <memory>

#include "ocr_backend.hpp"

namespace qs::windows::sys {

Ocr::Ocr(QObject* parent): QObject(parent), mBackend(std::make_unique<OcrBackend>(this)) {}

Ocr::~Ocr() = default;

int Ocr::recognizeText(const QString& path) {
	auto requestId = this->mNextRequestId++;
	this->mBackend->requestRecognize(requestId, path);
	return requestId;
}

void Ocr::backendDone(int requestId, const QString& text, bool ok, const QString& error) {
	emit this->recognized(requestId, text, ok, error);
}

} // namespace qs::windows::sys
