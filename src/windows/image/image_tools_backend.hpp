#pragma once

#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtmetamacros.h>

namespace qs::windows::image {

class ImageTools;
class ImageToolsWorker;

class ImageToolsBackend: public QObject {
	Q_OBJECT;

public:
	explicit ImageToolsBackend(ImageTools* frontend);
	~ImageToolsBackend() override;
	Q_DISABLE_COPY_MOVE(ImageToolsBackend);

	void requestLeastBusyRegion(
	    int requestId,
	    const QString& imagePath,
	    int width,
	    int height,
	    int screenWidth,
	    int screenHeight,
	    int horizontalPadding,
	    int verticalPadding,
	    bool busiest
	);

private:
	QThread mThread;
	ImageToolsWorker* mWorker = nullptr;
};

} // namespace qs::windows::image
