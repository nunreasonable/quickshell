#pragma once

#include <vector>

#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtmetamacros.h>
#include <qtypes.h>

class QImage;

namespace qs::windows::sys {

struct ClipboardEntry {
	qint64 id = 0;
	bool isImage = false;
	QString text; // full, unflattened text (empty for image entries)
	QString imagePath; // absolute path to a cached PNG (empty for text entries)
	int width = 0;
	int height = 0;
};

///! Own clipboard history (replaces cliphist/wl-clipboard). Captured via
/// `AddClipboardFormatListener`; text is kept in memory, images are saved as PNGs under the
/// cache directory. Mirrors the shape of ii's old cliphist-backed `Cliphist.qml` service:
/// `entries` is `"<id>\t<preview>"` for text and `"<id>\t[[ binary data WxH ]]"` for images,
/// same as a `cliphist list` line, so the existing fuzzy-search/list UI needs no changes.
class Clipboard: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	Q_PROPERTY(QStringList entries READ default NOTIFY entriesChanged BINDABLE bindableEntries);

public:
	explicit Clipboard(QObject* parent = nullptr);

	[[nodiscard]] QBindable<QStringList> bindableEntries() const { return &this->bEntries; }

	/// Writes entry `id`'s content back to the system clipboard (and moves it to the front of
	/// history, like selecting an item in cliphist would).
	Q_INVOKABLE void copy(qint64 id);
	Q_INVOKABLE void deleteEntry(qint64 id);
	Q_INVOKABLE void wipe();

	/// Absolute path to entry `id`'s cached PNG, or an empty string if it's not an image entry.
	Q_INVOKABLE QString imagePath(qint64 id) const;

	/// Writes the image file at `path` to the system clipboard (not added to history under its
	/// own id - the resulting WM_CLIPBOARDUPDATE round-trip captures it like any other image
	/// copy would). Used by the region selector in place of `wl-copy` on a cropped screenshot.
	/// Returns false if `path` can't be loaded as an image.
	Q_INVOKABLE bool copyImageFile(const QString& path);

	/// Writes `text` to the system clipboard, same as @@copy would for a text entry. Used by
	/// the region selector to place OCR results on the clipboard.
	Q_INVOKABLE void copyText(const QString& text);

signals:
	void entriesChanged();

private:
	void onClipboardUpdate();
	void rebuildEntriesProperty();
	void trimHistory();
	void captureText(const QString& text);
	void captureImage();
	QString cacheDir();

	// Assumes the clipboard is already open and emptied by the caller.
	void writeImageToClipboard(const QImage& image);
	void writeTextToClipboard(const QString& text);

	std::vector<ClipboardEntry> storage; // index 0 = most recent
	qint64 nextId = 1;
	qint64 suppressNextCaptureFor = -1;

	Q_OBJECT_BINDABLE_PROPERTY(Clipboard, QStringList, bEntries, &Clipboard::entriesChanged);
};

} // namespace qs::windows::sys
