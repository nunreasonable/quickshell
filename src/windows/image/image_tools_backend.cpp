#include "image_tools_backend.hpp"

#include <qmetaobject.h>
#include <qobject.h>
#include <qstring.h>
#include <qthread.h>

#include "image_tools.hpp"
#include "image_tools_worker.hpp"

namespace qs::windows::image {

ImageToolsBackend::ImageToolsBackend(ImageTools* frontend) {
	this->mWorker = new ImageToolsWorker(frontend);
	this->mWorker->moveToThread(&this->mThread);
	this->mThread.start();
}

ImageToolsBackend::~ImageToolsBackend() {
	this->mThread.quit();
	this->mThread.wait();
	delete this->mWorker;
}

void ImageToolsBackend::requestLeastBusyRegion(
    int requestId,
    const QString& imagePath,
    int width,
    int height,
    int screenWidth,
    int screenHeight,
    int horizontalPadding,
    int verticalPadding,
    bool busiest
) {
	auto* worker = this->mWorker;
	QMetaObject::invokeMethod(
	    worker,
	    [worker,
	     requestId,
	     imagePath,
	     width,
	     height,
	     screenWidth,
	     screenHeight,
	     horizontalPadding,
	     verticalPadding,
	     busiest] {
		    worker->cmdLeastBusyRegion(
		        requestId,
		        imagePath,
		        width,
		        height,
		        screenWidth,
		        screenHeight,
		        horizontalPadding,
		        verticalPadding,
		        busiest
		    );
	    },
	    Qt::QueuedConnection
	);
}

} // namespace qs::windows::image
