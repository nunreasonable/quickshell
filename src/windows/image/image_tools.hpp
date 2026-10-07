#pragma once

#include <memory>

#include <qobject.h>
#include <qsize.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qvariant.h>

namespace qs::windows::image {

class ImageToolsBackend;

class ImageTools: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit ImageTools(QObject* parent = nullptr);
	~ImageTools() override;
	Q_DISABLE_COPY_MOVE(ImageTools);

	Q_INVOKABLE static QVariantMap leastBusyRegion(
	    const QString& imagePath,
	    int width,
	    int height,
	    int screenWidth,
	    int screenHeight,
	    int horizontalPadding,
	    int verticalPadding,
	    bool busiest
	);

	Q_INVOKABLE static QVariantMap textColorFromImage(const QString& imagePath);

	Q_INVOKABLE static QString schemeForImage(const QString& imagePath);

	Q_INVOKABLE static QSize imageSize(const QString& imagePath);

	/// Asynchronous counterpart of `leastBusyRegion`. Runs on a dedicated worker thread,
	/// caches results by (path, mtime, size, parameters) and coalesces concurrent requests
	/// for the same key into a single computation. Returns a request id; the result for it
	/// arrives later via `leastBusyRegionReady`.
	Q_INVOKABLE int requestLeastBusyRegion(
	    const QString& imagePath,
	    int width,
	    int height,
	    int screenWidth,
	    int screenHeight,
	    int horizontalPadding,
	    int verticalPadding,
	    bool busiest
	);

	void backendDone(int requestId, const QVariantMap& result);

signals:
	void leastBusyRegionReady(int requestId, const QVariantMap& result);

private:
	std::unique_ptr<ImageToolsBackend> mBackend;
	int mNextRequestId = 1;
};

} // namespace qs::windows::image
