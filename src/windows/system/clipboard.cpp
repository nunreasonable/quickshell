#include "clipboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <qt_windows.h>

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qimage.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qstandardpaths.h>
#include <qstring.h>

#include "../../core/logcat.hpp"
#include "../services/message_window.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logClipboard, "quickshell.windows.clipboard", QtWarningMsg);

constexpr qsizetype MAX_ENTRIES = 200;

// Copied images, a "<pid>-<n>" folder per Clipboard: the history lives in memory, so whatever a
// process leaves behind (it crashed) is of no use to the next one.
QString cacheRoot() {
	return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
	     + QStringLiteral("/quickshell/clipboard");
}

bool processAlive(DWORD pid) {
	auto* process = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (process == nullptr) return false;
	auto alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
	CloseHandle(process);
	return alive;
}

void pruneStaleCaches() {
	auto root = QDir(cacheRoot());
	if (!root.exists()) return;

	for (const auto& entry: root.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot)) {
		auto ok = false;
		auto pid = entry.fileName().section('-', 0, 0).toULong(&ok);
		if (entry.isDir() && ok && processAlive(static_cast<DWORD>(pid))) continue;

		if (entry.isDir()) QDir(entry.absoluteFilePath()).removeRecursively();
		else QFile::remove(entry.absoluteFilePath());
	}
}

QString flattenForPreview(QString text) {
	text.replace(QStringLiteral("\r\n"), QStringLiteral(" ⏎ "));
	text.replace(QLatin1Char('\n'), QStringLiteral(" ⏎ "));
	return text.trimmed();
}

} // namespace

Clipboard::Clipboard(QObject* parent): QObject(parent) {
	static int instances = 0; // NOLINT
	if (instances == 0) pruneStaleCaches();
	this->mCacheDir = cacheRoot() + QStringLiteral("/%1-%2").arg(GetCurrentProcessId()).arg(instances++);

	auto* window = qs::windows::services::ServiceMessageWindow::instance();

	// A config reload creates the new singleton before the old one goes away, and the window
	// keeps both handlers; the listener stays registered for the window's lifetime.
	static auto listening = false; // NOLINT
	if (!listening) {
		listening = AddClipboardFormatListener(window->hwnd()) != FALSE;
		if (!listening) qCWarning(logClipboard) << "AddClipboardFormatListener failed:" << GetLastError();
	}

	window->addHandler(WM_CLIPBOARDUPDATE, [self = QPointer(this)](WPARAM /*w*/, LPARAM /*l*/) {
		if (self) self->onClipboardUpdate();
	});
}

Clipboard::~Clipboard() { QDir(this->mCacheDir).removeRecursively(); }

QString Clipboard::cacheDir() {
	QDir().mkpath(this->mCacheDir);
	return this->mCacheDir;
}

void Clipboard::onClipboardUpdate() {
	auto excludeFmt = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
	auto ignoreFmt = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
	auto historyOptFmt = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");

	if (IsClipboardFormatAvailable(excludeFmt) || IsClipboardFormatAvailable(ignoreFmt)) {
		return;
	}

	auto* window = qs::windows::services::ServiceMessageWindow::instance();
	if (!OpenClipboard(window->hwnd())) return;

	if (IsClipboardFormatAvailable(historyOptFmt)) {
		auto* data = GetClipboardData(historyOptFmt);
		auto optOut = false;
		if (data != nullptr) {
			auto* locked = GlobalLock(data);
			if (locked != nullptr) {
				optOut = *static_cast<const unsigned char*>(locked) == 0;
				GlobalUnlock(data);
			}
		}
		if (optOut) {
			CloseClipboard();
			return;
		}
	}

	if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
		auto* data = GetClipboardData(CF_UNICODETEXT);
		QString text;
		if (data != nullptr) {
			auto* locked = static_cast<const wchar_t*>(GlobalLock(data));
			if (locked != nullptr) {
				text = QString::fromWCharArray(locked);
				GlobalUnlock(data);
			}
		}
		CloseClipboard();
		if (!text.isEmpty()) this->captureText(text);
	} else if (IsClipboardFormatAvailable(CF_DIB)) {
		this->captureImage();
		CloseClipboard();
	} else {
		CloseClipboard();
	}
}

void Clipboard::captureText(const QString& text) {
	if (!this->storage.empty() && this->suppressNextCaptureFor == this->storage.front().id
	    && !this->storage.front().isImage && this->storage.front().text == text)
	{
		this->suppressNextCaptureFor = -1;
		return;
	}

	if (!this->storage.empty() && !this->storage.front().isImage
	    && this->storage.front().text == text)
	{
		return; // identical to the current top entry, nothing changed
	}

	ClipboardEntry entry;
	entry.id = this->nextId++;
	entry.isImage = false;
	entry.text = text;

	this->storage.insert(this->storage.begin(), entry);
	this->trimHistory();
	this->rebuildEntriesProperty();
}

// Clipboard DIBs can be any bit depth; blitting through GDI onto a fresh 32bpp top-down DIB
// section converts for us instead of hand-decoding every BITMAPINFOHEADER variant.
void Clipboard::captureImage() {
	auto* data = GetClipboardData(CF_DIB);
	if (data == nullptr) return;

	auto* bytes = static_cast<const unsigned char*>(GlobalLock(data));
	if (bytes == nullptr) return;

	const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(bytes);
	auto width = header->biWidth;
	auto height = std::abs(header->biHeight);

	if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
		GlobalUnlock(data);
		return;
	}

	auto* screenDc = GetDC(nullptr);

	BITMAPINFO outInfo {};
	outInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	outInfo.bmiHeader.biWidth = width;
	outInfo.bmiHeader.biHeight = -height; // negative: top-down, matches QImage's row order
	outInfo.bmiHeader.biPlanes = 1;
	outInfo.bmiHeader.biBitCount = 32;
	outInfo.bmiHeader.biCompression = BI_RGB;

	void* outBits = nullptr;
	auto* outDib = CreateDIBSection(screenDc, &outInfo, DIB_RGB_COLORS, &outBits, nullptr, 0);
	auto* memDc = CreateCompatibleDC(screenDc);
	auto* oldBmp = SelectObject(memDc, outDib);

	auto blitted = false;
	if (outDib != nullptr && outBits != nullptr) {
		blitted = StretchDIBits(
		              memDc,
		              0,
		              0,
		              width,
		              height,
		              0,
		              0,
		              width,
		              height,
		              bytes + header->biSize,
		              reinterpret_cast<const BITMAPINFO*>(header),
		              DIB_RGB_COLORS,
		              SRCCOPY
		          )
		       != GDI_ERROR;
	}

	QImage image;
	if (blitted) {
		// Format_RGB32 (not ARGB32): CF_DIB has no alpha channel, the 4th byte is unused/garbage.
		QImage view(
		    static_cast<uchar*>(outBits),
		    width,
		    height,
		    width * 4,
		    QImage::Format_RGB32
		);
		image = view.copy(); // own the pixels before the DIB section is destroyed below
	}

	SelectObject(memDc, oldBmp);
	DeleteDC(memDc);
	if (outDib != nullptr) DeleteObject(outDib);
	ReleaseDC(nullptr, screenDc);
	GlobalUnlock(data);

	if (image.isNull()) {
		qCWarning(logClipboard) << "Failed to convert clipboard DIB to an image";
		return;
	}

	auto id = this->nextId++;
	auto path = this->cacheDir() + QStringLiteral("/%1.png").arg(id);
	if (!image.save(path, "PNG")) {
		qCWarning(logClipboard) << "Failed to save clipboard image to" << path;
		return;
	}

	ClipboardEntry entry;
	entry.id = id;
	entry.isImage = true;
	entry.imagePath = path;
	entry.width = width;
	entry.height = height;

	this->storage.insert(this->storage.begin(), entry);
	this->trimHistory();
	this->rebuildEntriesProperty();
}

void Clipboard::trimHistory() {
	while (static_cast<qsizetype>(this->storage.size()) > MAX_ENTRIES) {
		const auto& evicted = this->storage.back();
		if (evicted.isImage && !evicted.imagePath.isEmpty()) QFile::remove(evicted.imagePath);
		this->storage.pop_back();
	}
}

void Clipboard::rebuildEntriesProperty() {
	QStringList lines;
	lines.reserve(static_cast<qsizetype>(this->storage.size()));

	for (const auto& entry: this->storage) {
		if (entry.isImage) {
			lines.append(
			    QStringLiteral("%1\t[[ binary data %2x%3 ]]")
			        .arg(entry.id)
			        .arg(entry.width)
			        .arg(entry.height)
			);
		} else {
			lines.append(QStringLiteral("%1\t%2").arg(entry.id).arg(flattenForPreview(entry.text)));
		}
	}

	this->bEntries = lines;
}

void Clipboard::writeImageToClipboard(const QImage& imageIn) {
	auto image = imageIn.convertToFormat(QImage::Format_RGB32);

	BITMAPINFOHEADER bih {};
	bih.biSize = sizeof(BITMAPINFOHEADER);
	bih.biWidth = image.width();
	bih.biHeight = image.height(); // bottom-up, classic CF_DIB convention
	bih.biPlanes = 1;
	bih.biBitCount = 32;
	bih.biCompression = BI_RGB;

	auto pixelBytes = static_cast<SIZE_T>(image.width()) * image.height() * 4;
	auto* mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes);
	if (mem == nullptr) return;

	auto* dst = static_cast<unsigned char*>(GlobalLock(mem));
	if (dst == nullptr) {
		GlobalFree(mem);
		return;
	}

	memcpy(dst, &bih, sizeof(bih)); // NOLINT
	// CF_DIB rows are bottom-up; QImage rows are top-down.
	for (int y = 0; y < image.height(); y++) {
		memcpy( // NOLINT
		    dst + sizeof(bih) + static_cast<SIZE_T>(y) * image.width() * 4,
		    image.constScanLine(image.height() - 1 - y),
		    static_cast<SIZE_T>(image.width()) * 4
		);
	}
	GlobalUnlock(mem);
	// The clipboard owns the memory only once SetClipboardData succeeds.
	if (SetClipboardData(CF_DIB, mem) == nullptr) GlobalFree(mem);
}

void Clipboard::writeTextToClipboard(const QString& text) {
	auto bytes = (text.size() + 1) * sizeof(wchar_t);
	auto* mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (mem == nullptr) return;

	auto* dst = GlobalLock(mem);
	if (dst == nullptr) {
		GlobalFree(mem);
		return;
	}

	memcpy(dst, text.utf16(), bytes); // NOLINT
	GlobalUnlock(mem);
	if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) GlobalFree(mem);
}

void Clipboard::copy(qint64 id) {
	auto iter = std::find_if(this->storage.begin(), this->storage.end(), [id](const auto& e) {
		return e.id == id;
	});
	if (iter == this->storage.end()) return;

	auto entry = *iter;
	this->storage.erase(iter);
	this->storage.insert(this->storage.begin(), entry);
	this->rebuildEntriesProperty();

	auto* window = qs::windows::services::ServiceMessageWindow::instance();
	if (!OpenClipboard(window->hwnd())) return;
	EmptyClipboard();

	if (entry.isImage) {
		QImage image(entry.imagePath);
		if (!image.isNull()) this->writeImageToClipboard(image);
	} else {
		this->writeTextToClipboard(entry.text);
	}

	CloseClipboard();
	this->suppressNextCaptureFor = id;
}

bool Clipboard::copyImageFile(const QString& path) {
	QImage image(path);
	if (image.isNull()) {
		qCWarning(logClipboard) << "Cannot load image" << path;
		return false;
	}

	auto* window = qs::windows::services::ServiceMessageWindow::instance();
	if (!OpenClipboard(window->hwnd())) return false;
	EmptyClipboard();
	this->writeImageToClipboard(image);
	CloseClipboard();
	return true;
}

void Clipboard::copyText(const QString& text) {
	auto* window = qs::windows::services::ServiceMessageWindow::instance();
	if (!OpenClipboard(window->hwnd())) return;
	EmptyClipboard();
	this->writeTextToClipboard(text);
	CloseClipboard();
}

void Clipboard::deleteEntry(qint64 id) {
	auto iter = std::find_if(this->storage.begin(), this->storage.end(), [id](const auto& e) {
		return e.id == id;
	});
	if (iter == this->storage.end()) return;

	if (iter->isImage && !iter->imagePath.isEmpty()) QFile::remove(iter->imagePath);
	this->storage.erase(iter);
	this->rebuildEntriesProperty();
}

void Clipboard::wipe() {
	for (const auto& entry: this->storage) {
		if (entry.isImage && !entry.imagePath.isEmpty()) QFile::remove(entry.imagePath);
	}
	this->storage.clear();
	this->rebuildEntriesProperty();
}

QString Clipboard::imagePath(qint64 id) const {
	auto iter = std::find_if(this->storage.begin(), this->storage.end(), [id](const auto& e) {
		return e.id == id;
	});
	if (iter == this->storage.end() || !iter->isImage) return QString();
	return iter->imagePath;
}

} // namespace qs::windows::sys
