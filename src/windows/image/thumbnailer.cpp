#include "thumbnailer.hpp"

#include <algorithm>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qimage.h>
#include <qimagereader.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qpointer.h>
#include <qrunnable.h>
#include <qsize.h>
#include <qthreadpool.h>
#include <quuid.h>

namespace qs::windows::image {

namespace {
Q_LOGGING_CATEGORY(logThumbnailer, "quickshell.windows.thumbnailer", QtWarningMsg);

bool saveAtomically(const QImage& image, const QString& outputPath) {
	auto dir = QFileInfo(outputPath).absoluteDir();
	if (!dir.exists() && !dir.mkpath(".")) {
		qCWarning(logThumbnailer) << "could not create" << dir.absolutePath();
		return false;
	}

	auto tempPath = outputPath + u".tmp-" + QUuid::createUuid().toString(QUuid::Id128);
	if (!image.save(tempPath, "PNG")) {
		qCWarning(logThumbnailer) << "could not write" << tempPath;
		QFile::remove(tempPath);
		return false;
	}

	QFile::remove(outputPath);
	if (!QFile::rename(tempPath, outputPath)) {
		qCWarning(logThumbnailer) << "could not move" << tempPath << "to" << outputPath;
		QFile::remove(tempPath);
		return false;
	}

	return true;
}

bool writeThumbnail(const QString& sourcePath, const QString& outputPath, int maxSize) {
	QImageReader reader(sourcePath);
	reader.setAutoTransform(true);

	auto size = reader.size();
	if (size.isValid()) {
		reader.setScaledSize(size.scaled(maxSize, maxSize, Qt::KeepAspectRatio));
	}

	auto image = reader.read();
	if (image.isNull()) {
		qCWarning(logThumbnailer) << "could not decode" << sourcePath << ":" << reader.errorString();
		return false;
	}

	if (!size.isValid()) {
		image = image.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}

	return saveAtomically(image, outputPath);
}

class ThumbnailTask: public QRunnable {
public:
	ThumbnailTask(Thumbnailer* owner, QString source, QString output, int maxSize)
	    : owner(owner)
	    , source(std::move(source))
	    , output(std::move(output))
	    , maxSize(maxSize) {}

	void run() override {
		auto ok = writeThumbnail(this->source, this->output, this->maxSize);
		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [owner = this->owner, source = this->source, output = this->output, ok] {
			    if (owner) emit owner->finished(source, output, ok);
		    },
		    Qt::QueuedConnection
		);
	}

private:
	QPointer<Thumbnailer> owner;
	QString source;
	QString output;
	int maxSize;
};

} // namespace

void Thumbnailer::generate(const QString& sourcePath, const QString& outputPath, int maxSize) {
	if (QFileInfo::exists(outputPath)) {
		emit this->finished(sourcePath, outputPath, true);
		return;
	}

	auto* task = new ThumbnailTask(this, sourcePath, outputPath, maxSize); // NOLINT
	task->setAutoDelete(true);
	QThreadPool::globalInstance()->start(task);
}

} // namespace qs::windows::image
