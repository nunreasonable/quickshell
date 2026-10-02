#include "screenshot.hpp"

#include <qt_windows.h>

#include <qdir.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qimage.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpainter.h>
#include <qrect.h>
#include <qscreen.h>
#include <qstring.h>

#include "../capture.hpp"
#include "../util.hpp"

namespace qs::windows::sys {

namespace {
Q_LOGGING_CATEGORY(logScreenshot, "quickshell.windows.screenshot", QtWarningMsg);
}

bool Screenshot::captureScreen(const QString& screenName, const QString& path) {
	QList<QScreen*> screens;

	for (auto* screen: QGuiApplication::screens()) {
		if (screenName.isEmpty() || screen->name() == screenName) screens.append(screen);
	}

	if (screens.isEmpty()) {
		qCWarning(logScreenshot) << "No screen named" << screenName;
		return false;
	}

	struct Shot {
		QRect rect; // physical pixels, desktop coordinates
		QImage image;
	};

	QList<Shot> shots;
	QRect bounds;

	for (auto* screen: screens) {
		auto* monitor = monitorForScreen(screen);
		auto rects = monitorRects(monitor);

		if (monitor == nullptr || !rects.valid) {
			qCWarning(logScreenshot) << "No monitor for screen" << screen->name();
			return false;
		}

		auto image = capture::CaptureThread::instance()->grabMonitor(monitor);
		if (image.isNull()) {
			qCWarning(logScreenshot) << "Capturing" << screen->name() << "failed";
			return false;
		}

		shots.append({.rect = QRect(rects.monitor.topLeft(), image.size()), .image = image});
		bounds = bounds.united(shots.last().rect);
	}

	QImage result;

	if (shots.size() == 1) {
		result = shots.first().image;
	} else {
		result = QImage(bounds.size(), QImage::Format_RGBX8888);
		result.fill(Qt::black);
		QPainter painter(&result);
		for (const auto& shot: shots) painter.drawImage(shot.rect.topLeft() - bounds.topLeft(), shot.image);
	}

	auto info = QFileInfo(path);
	if (!QDir().mkpath(info.absolutePath())) {
		qCWarning(logScreenshot) << "Cannot create" << info.absolutePath();
		return false;
	}

	if (!result.save(path, "PNG")) {
		qCWarning(logScreenshot) << "Cannot write" << path;
		return false;
	}

	return true;
}

} // namespace qs::windows::sys
