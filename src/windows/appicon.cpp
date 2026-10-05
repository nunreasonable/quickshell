#include "appicon.hpp"

#include <qcache.h>
#include <qimage.h>
#include <qloggingcategory.h>
#include <qmutex.h>
#include <qpixmap.h>
#include <qsize.h>
#include <qstring.h>
#include <qstringlist.h>

#include <qt_windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "desktopentry_backend.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logAppIcon, "quickshell.windows.appicon", QtWarningMsg);

struct ComApartment {
	ComApartment(): hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}

	ComApartment(const ComApartment&) = delete;
	ComApartment(ComApartment&&) = delete;
	ComApartment& operator=(const ComApartment&) = delete;
	ComApartment& operator=(ComApartment&&) = delete;

	~ComApartment() {
		if (SUCCEEDED(this->hr)) CoUninitialize();
	}

	HRESULT hr;
};

bool looksLikeWindowsPath(const QString& s) {
	return s.length() > 2 && s[0].isLetter() && s[1] == u':' && (s[2] == u'/' || s[2] == u'\\');
}

bool isPlainImage(const QString& path) {
	static const auto suffixes = QStringList {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".svg"};

	for (const auto& suffix: suffixes) {
		if (path.endsWith(suffix, Qt::CaseInsensitive)) return true;
	}

	return false;
}

QImage hbitmapToImage(HBITMAP bitmap) {
	auto bm = BITMAP {};
	if (GetObjectW(bitmap, sizeof(bm), &bm) == 0) return QImage();

	auto width = bm.bmWidth;
	auto height = bm.bmHeight;
	if (width <= 0 || height <= 0) return QImage();

	auto bmi = BITMAPINFO {};
	bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -height;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	auto image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	if (image.isNull()) return image;

	auto* hdc = GetDC(nullptr);
	auto ok = GetDIBits(hdc, bitmap, 0, static_cast<UINT>(height), image.bits(), &bmi, DIB_RGB_COLORS);
	ReleaseDC(nullptr, hdc);

	if (ok == 0) return QImage();

	return image.convertToFormat(QImage::Format_ARGB32);
}

QPixmap renderShellItemIcon(IShellItem* item, const QSize& size) {
	ComPtr<IShellItemImageFactory> factory;
	if (FAILED(item->QueryInterface(IID_PPV_ARGS(&factory)))) return QPixmap();

	auto width = size.width() > 0 ? size.width() : 48;
	auto height = size.height() > 0 ? size.height() : 48;
	auto requested = SIZE {.cx = width, .cy = height};

	HBITMAP bitmap = nullptr;
	auto hr = factory->GetImage(requested, SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT, &bitmap);
	if (FAILED(hr) || bitmap == nullptr) return QPixmap();

	auto image = hbitmapToImage(bitmap);
	DeleteObject(bitmap);
	return QPixmap::fromImage(image);
}

QPixmap renderParsingName(const QString& parsingName, const QSize& size) {
	auto wname = parsingName.toStdWString();
	ComPtr<IShellItem> item;
	auto hr = SHCreateItemFromParsingName(wname.c_str(), nullptr, IID_PPV_ARGS(&item));

	if (FAILED(hr)) {
		qCDebug(logAppIcon) << "SHCreateItemFromParsingName failed for" << parsingName << ":"
		                    << Qt::hex << hr;
		return QPixmap();
	}

	return renderShellItemIcon(item.Get(), size);
}

QMutex cacheMutex;
QCache<QString, QPixmap> cache(256); // NOLINT

QString cacheKeyFor(const QString& parsingName, const QSize& size) {
	return parsingName + u'@' + QString::number(size.width()) + u'x' + QString::number(size.height());
}

} // namespace

QPixmap iconForKey(const QString& key, const QSize& size) {
	QString parsingName;

	static const auto appIconPrefix = QStringLiteral("appicon:");
	if (key.startsWith(appIconPrefix)) {
		auto id = key.sliced(appIconPrefix.length());
		auto token = WindowsDesktopEntryBackend::parsingNameForId(id);
		if (token.isEmpty()) return QPixmap();
		parsingName = QStringLiteral("shell:AppsFolder\\") + token;
	} else if (looksLikeWindowsPath(key)) {
		if (isPlainImage(key)) return QPixmap();
		parsingName = key;
		parsingName.replace(u'/', u'\\');
	} else {
		return QPixmap();
	}

	auto key2 = cacheKeyFor(parsingName, size);

	{
		QMutexLocker locker(&cacheMutex);
		if (auto* hit = cache.object(key2)) return *hit;
	}

	auto com = ComApartment();
	if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) {
		qCWarning(logAppIcon) << "CoInitializeEx failed:" << Qt::hex << com.hr;
		return QPixmap();
	}

	auto pixmap = renderParsingName(parsingName, size);
	if (pixmap.isNull()) return pixmap;

	{
		QMutexLocker locker(&cacheMutex);
		cache.insert(key2, new QPixmap(pixmap));
	}

	return pixmap;
}

} // namespace qs::windows
