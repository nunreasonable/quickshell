#pragma once

#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "tiling_layout.hpp"

namespace qs::windows {

class TrackedWindow;
class WindowTracker;
class VirtualDesktops;

class TilingManager: public QObject {
	Q_OBJECT;

public:
	using Edge = tiling::Edge;

	static TilingManager* instance();
	static TilingManager* active();

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);
	[[nodiscard]] qint32 gapsIn() const { return this->mGapsIn; }
	void setGapsIn(qint32 gaps);
	[[nodiscard]] qint32 gapsOut() const { return this->mGapsOut; }
	void setGapsOut(qint32 gaps);
	[[nodiscard]] bool preserveSplit() const { return this->mPreserveSplit; }
	void setPreserveSplit(bool preserve);
	[[nodiscard]] QStringList excluded() const { return this->mExcluded; }
	void setExcluded(const QStringList& excluded);

	void relayout();

	[[nodiscard]] bool isTiled(TrackedWindow* window) const;

	bool focusDirection(Edge direction);
	bool moveDirection(Edge direction);
	bool swapDirection(Edge direction);
	bool toggleSplit(TrackedWindow* window);
	bool swapSplit(TrackedWindow* window);
	bool splitRatio(TrackedWindow* window, double value, bool exact);
	bool resizeTiled(TrackedWindow* window, qint32 dx, qint32 dy);
	bool setFloating(TrackedWindow* window, bool floating, bool toggle);

	bool beginEdgeDrag(TrackedWindow* window, bool left, bool top);
	void edgeDrag(const QPoint& delta);
	void endEdgeDrag();

	static void resizeFloating(TrackedWindow* window, qint32 dx, qint32 dy, bool exact);
	static void center(TrackedWindow* window);

signals:
	void enabledChanged();
	void gapsInChanged();
	void gapsOutChanged();
	void preserveSplitChanged();
	void excludedChanged();
	void windowTilingChanged(TrackedWindow* window);

private:
	explicit TilingManager();
	~TilingManager() override = default;
	Q_DISABLE_COPY_MOVE(TilingManager);

	struct Layout {
		GUID desktop {};
		QPointer<QScreen> screen;
		tiling::DwindleLayout tree;
		TrackedWindow* lastFocused = nullptr;
	};

	enum class State : quint8 {
		Out,
		Hidden,
		Suspended,
		Tiled,
	};

	enum class Override : quint8 { None, Float, Tile };

	struct EdgeDrag {
		TrackedWindow* window = nullptr;
		Layout* layout = nullptr;
		QRect box;
		std::optional<Edge> horizontal;
		std::optional<Edge> vertical;
	};

	struct Managed {
		TrackedWindow* window = nullptr;
		Layout* layout = nullptr;
		Override override = Override::None;
		bool elevated = false;
		bool autoFloat = false;
		QRect target;
		QRect before;
		QRect accepted;
		qint32 corrections = 0;
		qint64 lastCorrection = 0;
		bool verifying = false;
		qint64 deadline = 0;
		bool parked = false;
		QRect preTiling;
		quint64 focusStamp = 0;
	};

	bool start();
	void stop();
	void manage(TrackedWindow* window);
	void unmanage(TrackedWindow* window);

	[[nodiscard]] State stateOf(const Managed& m) const;
	[[nodiscard]] bool floatsByRule(const Managed& m) const;
	[[nodiscard]] bool isExcluded(TrackedWindow* window) const;
	[[nodiscard]] Layout* layoutFor(TrackedWindow* window, bool create);
	[[nodiscard]] Layout* layoutAt(const GUID& desktop, QScreen* screen, bool create);
	[[nodiscard]] bool layoutAlive(const Layout* layout) const;
	[[nodiscard]] static tiling::DwindleLayout::Id idOf(TrackedWindow* window);
	[[nodiscard]] TrackedWindow* windowOf(tiling::DwindleLayout::Id id) const;

	void markWindow(TrackedWindow* window);
	void markLayout(Layout* layout);
	void schedule();
	void sync();
	void syncWindow(TrackedWindow* window);
	void insertInto(Managed& m, Layout* layout, TrackedWindow* target, const QPoint* cursor);
	void takeOut(Managed& m);
	void dropEmptyLayouts();
	void apply(Layout* layout);
	[[nodiscard]] QRect tileRect(const Layout* layout, TrackedWindow* window) const;
	void place(Managed& m, const QRect& tile);
	void send(Managed& m, const QRect& frame);
	void armDeadline();
	void restoreFloating(Managed& m);

	void onActiveWindowChanged();
	void onRectChanged(TrackedWindow* window);
	void onMoveSizeStarted(TrackedWindow* window);
	void onMoveSizeEnded(TrackedWindow* window);
	void onScreensChanged();
	void verify();
	void onDeadline();
	void drop(Managed& m, const QPoint& cursor);
	void giveUp(Managed& m, const QRect& frame);
	void markAll();

	[[nodiscard]] Managed* tiledActive();
	[[nodiscard]] Managed* neighbour(const Managed& from, Edge direction);
	[[nodiscard]] QScreen* screenInDirection(QScreen* from, Edge direction) const;
	void swapWindows(Managed& a, Managed& b);
	void moveToLayout(Managed& m, Layout* layout, TrackedWindow* target);

	WindowTracker* tracker = nullptr;
	VirtualDesktops* desktops = nullptr;

	bool mEnabled = false;
	qint32 mGapsIn = 4;
	qint32 mGapsOut = 5;
	bool mPreserveSplit = true;
	QStringList mExcluded;
	QList<QRegularExpression> excludedPatterns;
	QList<QRegularExpression> excludedTitles;

	std::unordered_map<TrackedWindow*, Managed> managed;
	std::map<QString, std::unique_ptr<Layout>> layouts;
	QSet<TrackedWindow*> dirtyWindows;
	QSet<Layout*> dirtyLayouts;
	QSet<TrackedWindow*> unverified;
	TrackedWindow* dragging = nullptr;
	EdgeDrag edgeDragState;
	quint64 focusCounter = 0;

	HANDLE ownerMutex = nullptr;
	QTimer syncTimer;
	QTimer verifyTimer;
	QTimer deadlineTimer;
	QList<QMetaObject::Connection> connections;
};

class Tiling: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	Q_PROPERTY(qint32 gapsIn READ gapsIn WRITE setGapsIn NOTIFY gapsInChanged);
	Q_PROPERTY(qint32 gapsOut READ gapsOut WRITE setGapsOut NOTIFY gapsOutChanged);
	Q_PROPERTY(bool preserveSplit READ preserveSplit WRITE setPreserveSplit NOTIFY preserveSplitChanged);
	Q_PROPERTY(QStringList excluded READ excluded WRITE setExcluded NOTIFY excludedChanged);
	// clang-format on

public:
	explicit Tiling(QObject* parent = nullptr);

	Q_INVOKABLE void relayout();

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool enabled);
	[[nodiscard]] qint32 gapsIn() const;
	void setGapsIn(qint32 gaps);
	[[nodiscard]] qint32 gapsOut() const;
	void setGapsOut(qint32 gaps);
	[[nodiscard]] bool preserveSplit() const;
	void setPreserveSplit(bool preserve);
	[[nodiscard]] QStringList excluded() const;
	void setExcluded(const QStringList& excluded);

signals:
	void enabledChanged();
	void gapsInChanged();
	void gapsOutChanged();
	void preserveSplitChanged();
	void excludedChanged();
};

} // namespace qs::windows
