#include "tiling.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <qdatetime.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpoint.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qtypes.h>

#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

// last: pulls in the rpc headers, which define macros like `small`
#include <dwmapi.h>

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logTiling, "quickshell.windows.tiling", QtWarningMsg);

using tiling::Edge;

// Window events arrive in bursts (a window shows, gets activated, moves itself); one layout
// pass covers the burst.
constexpr int SYNC_DELAY_MS = 30;
// Applications answer an asynchronous move some time later, sometimes in several steps (a DPI
// change resizes the window once more); checked once things have calmed down.
constexpr int VERIFY_DELAY_MS = 150;
// A window that hasn't moved at all by then ignores us (UIPI the integrity check missed, or an
// application that undoes every move).
constexpr int DEADLINE_MS = 2000;
// Corrective placements before a window that doesn't take its tile is left alone or floated.
// Two cover a move to a monitor with another DPI (the window rescales itself once).
constexpr qint32 MAX_CORRECTIONS = 2;
// Corrections further apart than this start counting again (the user snapping a tiled window
// away with Win+Arrow now and then is put back every time).
constexpr qint64 CORRECTION_RESET_MS = 1500;
// Physical pixels within which an edge counts as not moved by the user.
constexpr int EDGE_TOLERANCE = 2;

TilingManager* gManager = nullptr; // NOLINT

int rightOf(const QRect& rect) { return rect.left() + rect.width(); }
int bottomOf(const QRect& rect) { return rect.top() + rect.height(); }

QString windowClass(HWND hwnd) {
	wchar_t buffer[128] {};
	auto length = GetClassNameW(hwnd, buffer, 128);
	return QString::fromWCharArray(buffer, length);
}

// Raw window rect and visible frame (without the invisible resize borders), physical pixels.
bool readFrame(HWND hwnd, QRect& raw, QRect& frame) {
	RECT rawRect {};
	if (!GetWindowRect(hwnd, &rawRect)) return false;

	RECT frameRect {};
	auto hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect));
	if (FAILED(hr)) frameRect = rawRect;

	raw = toQRect(rawRect);
	frame = toQRect(frameRect);
	return true;
}

QRect currentFrame(HWND hwnd) {
	QRect raw;
	QRect frame;
	readFrame(hwnd, raw, frame);
	return frame;
}

// Asks the window to put its visible frame exactly on `target`. The invisible borders are
// whatever the window has now; after a move to a monitor with another DPI they change and the
// verification pass corrects the result once.
void setFrame(HWND hwnd, const QRect& target) {
	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return;

	auto left = frame.left() - raw.left();
	auto top = frame.top() - raw.top();
	auto right = rightOf(raw) - rightOf(frame);
	auto bottom = bottomOf(raw) - bottomOf(frame);

	// Asynchronous: the request is posted to the window's thread, so a hung application can't
	// block the shell.
	SetWindowPos(
	    hwnd,
	    nullptr,
	    target.left() - left,
	    target.top() - top,
	    target.width() + left + right,
	    target.height() + top + bottom,
	    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	);
}

void setRawRect(HWND hwnd, const QRect& rect) {
	SetWindowPos(
	    hwnd,
	    nullptr,
	    rect.left(),
	    rect.top(),
	    rect.width(),
	    rect.height(),
	    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	);
}

DWORD tokenIntegrity(HANDLE token) {
	DWORD size = 0;
	GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
	if (size == 0) return 0;

	auto buffer = QByteArray(static_cast<qsizetype>(size), '\0');
	auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
	if (!GetTokenInformation(token, TokenIntegrityLevel, label, size, &size)) return 0;

	auto* sid = label->Label.Sid;
	auto count = *GetSidSubAuthorityCount(sid);
	if (count == 0) return 0;
	return *GetSidSubAuthority(sid, count - 1);
}

DWORD ownIntegrity() {
	static const DWORD level = []() -> DWORD {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
			return SECURITY_MANDATORY_MEDIUM_RID;
		}
		auto level = tokenIntegrity(token);
		CloseHandle(token);
		return level == 0 ? SECURITY_MANDATORY_MEDIUM_RID : level;
	}();

	return level;
}

// UIPI drops SetWindowPos on windows of processes with a higher integrity level (elevated
// apps, Task Manager), so those float from the start instead of leaving a hole in the layout.
bool isMoreElevated(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	auto own = ownIntegrity();

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	// Not even limited access: a protected or system process, which is more than we are
	// unless the shell itself runs elevated.
	if (process == nullptr) return own < SECURITY_MANDATORY_HIGH_RID;

	HANDLE token = nullptr;
	auto opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
	CloseHandle(process);
	if (!opened) return own < SECURITY_MANDATORY_HIGH_RID;

	auto level = tokenIntegrity(token);
	CloseHandle(token);
	return level > own;
}

QPoint cursorPos() {
	POINT point {};
	GetCursorPos(&point);
	return {point.x, point.y};
}

QRect physicalMonitor(QScreen* screen) { return monitorRects(monitorForScreen(screen)).monitor; }

qint32 toPhysical(qint32 logical, qreal dpr) {
	return static_cast<qint32>(std::lround(logical * dpr));
}

} // namespace

// --- TilingManager -----------------------------------------------------------------------------

TilingManager* TilingManager::instance() {
	static auto* instance = new TilingManager(); // NOLINT
	return instance;
}

TilingManager* TilingManager::active() {
	return gManager != nullptr && gManager->mEnabled ? gManager : nullptr;
}

TilingManager::TilingManager() {
	gManager = this;

	this->syncTimer.setSingleShot(true);
	this->syncTimer.setInterval(SYNC_DELAY_MS);
	QObject::connect(&this->syncTimer, &QTimer::timeout, this, &TilingManager::sync);

	this->verifyTimer.setSingleShot(true);
	this->verifyTimer.setInterval(VERIFY_DELAY_MS);
	QObject::connect(&this->verifyTimer, &QTimer::timeout, this, &TilingManager::verify);

	this->deadlineTimer.setSingleShot(true);
	this->deadlineTimer.setInterval(DEADLINE_MS);
	QObject::connect(&this->deadlineTimer, &QTimer::timeout, this, &TilingManager::onDeadline);
}

void TilingManager::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;

	if (enabled) {
		if (!this->start()) {
			this->mEnabled = false;
			return;
		}
	} else {
		this->stop();
	}

	emit this->enabledChanged();
}

void TilingManager::setGapsIn(qint32 gaps) {
	gaps = qMax(0, gaps);
	if (gaps == this->mGapsIn) return;
	this->mGapsIn = gaps;
	this->markAll();
	emit this->gapsInChanged();
}

void TilingManager::setGapsOut(qint32 gaps) {
	gaps = qMax(0, gaps);
	if (gaps == this->mGapsOut) return;
	this->mGapsOut = gaps;
	this->markAll();
	emit this->gapsOutChanged();
}

void TilingManager::setPreserveSplit(bool preserve) {
	if (preserve == this->mPreserveSplit) return;
	this->mPreserveSplit = preserve;
	this->markAll();
	emit this->preserveSplitChanged();
}

void TilingManager::setExcluded(const QStringList& excluded) {
	if (excluded == this->mExcluded) return;
	this->mExcluded = excluded;

	this->excludedPatterns.clear();
	this->excludedTitles.clear();

	for (const auto& entry: excluded) {
		auto pattern = entry.trimmed();
		auto byTitle = pattern.startsWith("title:", Qt::CaseInsensitive);
		if (byTitle) pattern = pattern.mid(6).trimmed();
		if (pattern.isEmpty()) continue;

		auto expression = QRegularExpression(
		    QRegularExpression::wildcardToRegularExpression(
		        pattern,
		        QRegularExpression::NonPathWildcardConversion
		    ),
		    QRegularExpression::CaseInsensitiveOption
		);

		if (byTitle) this->excludedTitles.append(expression);
		else this->excludedPatterns.append(expression);
	}

	this->markAll();
	emit this->excludedChanged();
}

void TilingManager::relayout() {
	if (!this->mEnabled) return;

	for (auto& [window, m]: this->managed) {
		m.target = QRect();
		m.accepted = QRect();
		m.corrections = 0;
	}

	this->markAll();
}

void TilingManager::markAll() {
	if (!this->mEnabled) return;
	for (auto& [window, m]: this->managed) this->dirtyWindows.insert(window);
	for (auto& [key, layout]: this->layouts) this->dirtyLayouts.insert(layout.get());
	this->schedule();
}

bool TilingManager::isTiled(TrackedWindow* window) const {
	auto iter = this->managed.find(window);
	return iter != this->managed.end() && iter->second.layout != nullptr;
}

// --- lifecycle ---------------------------------------------------------------------------------

bool TilingManager::start() {
	// Two tiling processes (the shell and, say, a settings window of the same config) would
	// fight over every window. The first one to turn tiling on owns it for the session.
	this->ownerMutex = CreateMutexW(nullptr, TRUE, L"Local\\QuickshellTiling");
	if (this->ownerMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
		qCWarning(logTiling) << "Another Quickshell process already tiles windows;"
		                     << "tiling stays off in this one.";
		if (this->ownerMutex != nullptr) CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
		return false;
	}

	this->tracker = WindowTracker::instance();
	this->desktops = this->tracker->desktops();

	auto& c = this->connections;
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowAdded, this, &TilingManager::manage)
	);
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowRemoved, this, &TilingManager::unmanage)
	);
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::activeWindowChanged,
	    this,
	    &TilingManager::onActiveWindowChanged
	));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::moveSizeStarted,
	    this,
	    &TilingManager::onMoveSizeStarted
	));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::moveSizeEnded,
	    this,
	    &TilingManager::onMoveSizeEnded
	));
	// Removed desktops take their layouts with them; their windows land on another one.
	c.append(QObject::connect(this->desktops, &VirtualDesktops::desktopsChanged, this, [this]() {
		this->markAll();
	}));

	if (auto* app = qobject_cast<QGuiApplication*>(QGuiApplication::instance())) {
		auto changed = &TilingManager::onScreensChanged;
		c.append(QObject::connect(app, &QGuiApplication::screenAdded, this, changed));
		c.append(QObject::connect(app, &QGuiApplication::screenRemoved, this, changed));
	}

	this->onScreensChanged();

	for (auto* window: this->tracker->windows()) this->manage(window);

	// Windows already open are inserted left to right, top to bottom, each splitting the one
	// before it on its desktop and monitor, so a rough arrangement the user had survives.
	auto windows = this->tracker->windows();
	std::stable_sort(windows.begin(), windows.end(), [](TrackedWindow* a, TrackedWindow* b) {
		auto ca = a->rect().center();
		auto cb = b->rect().center();
		return ca.x() != cb.x() ? ca.x() < cb.x() : ca.y() < cb.y();
	});

	QHash<Layout*, TrackedWindow*> previous;
	for (auto* window: windows) {
		auto& m = this->managed.at(window);
		if (this->stateOf(m) != State::Tiled) continue;

		auto* layout = this->layoutFor(window, true);
		if (layout == nullptr) continue;

		this->insertInto(m, layout, previous.value(layout), nullptr);
		previous.insert(layout, window);
	}

	this->onActiveWindowChanged();
	this->schedule();
	return true;
}

void TilingManager::stop() {
	for (const auto& connection: this->connections) QObject::disconnect(connection);
	this->connections.clear();

	for (auto* screen: QGuiApplication::screens()) {
		QObject::disconnect(screen, nullptr, this, nullptr);
	}

	// Tiled windows go back to where they were before tiling; the rest stay as they are.
	QList<TrackedWindow*> tiled;
	for (auto& [window, m]: this->managed) {
		QObject::disconnect(window, nullptr, this, nullptr);
		if (m.layout == nullptr) continue;

		tiled.append(window);
		if (this->stateOf(m) == State::Tiled) this->restoreFloating(m);
	}

	this->managed.clear();
	this->layouts.clear();
	this->dirtyWindows.clear();
	this->dirtyLayouts.clear();
	this->unverified.clear();
	this->dragging = nullptr;
	this->syncTimer.stop();
	this->verifyTimer.stop();
	this->deadlineTimer.stop();

	if (this->ownerMutex != nullptr) {
		ReleaseMutex(this->ownerMutex);
		CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
	}

	for (auto* window: tiled) emit this->windowTilingChanged(window);
}

void TilingManager::manage(TrackedWindow* window) {
	if (this->managed.contains(window)) return;

	auto& m = this->managed[window];
	m.window = window;
	m.elevated = isMoreElevated(window->hwnd());

	if (m.elevated) {
		qCDebug(logTiling) << "Window" << window->addressHex() << window->appId()
		                   << "belongs to a more privileged process; it floats.";
	}

	auto mark = [this, window]() { this->markWindow(window); };
	QObject::connect(window, &TrackedWindow::minimizedChanged, this, mark);
	QObject::connect(window, &TrackedWindow::maximizedChanged, this, mark);
	QObject::connect(window, &TrackedWindow::fullscreenChanged, this, mark);
	QObject::connect(window, &TrackedWindow::desktopChanged, this, mark);
	QObject::connect(window, &TrackedWindow::screenChanged, this, mark);
	// Title rules (a "Picture in picture" window, a settings window) can match later.
	QObject::connect(window, &TrackedWindow::titleChanged, this, [this, window]() {
		if (!this->excludedTitles.isEmpty()) this->markWindow(window);
	});
	QObject::connect(window, &TrackedWindow::rectChanged, this, [this, window]() {
		this->onRectChanged(window);
	});

	this->markWindow(window);
}

void TilingManager::unmanage(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	QObject::disconnect(window, nullptr, this, nullptr);
	this->takeOut(iter->second);
	this->managed.erase(iter);

	this->dirtyWindows.remove(window);
	this->unverified.remove(window);
	if (this->dragging == window) this->dragging = nullptr;

	for (auto& [key, layout]: this->layouts) {
		if (layout->lastFocused == window) layout->lastFocused = nullptr;
	}
}

// --- classification ----------------------------------------------------------------------------

TilingManager::State TilingManager::stateOf(const Managed& m) const {
	auto* window = m.window;

	if (window->desktop() < 0 || window->screen() == nullptr) return State::Out;
	if (m.elevated) return State::Out;
	if (m.override == Override::Float) return State::Out;
	if (m.override != Override::Tile && (m.autoFloat || this->floatsByRule(m))) return State::Out;

	if (window->minimized()) return State::Hidden;
	if (window->maximized()) return State::Suspended;

	// The tracker calls a window covering its monitor fullscreen; a lone tile without gaps or
	// bars covers it too, and that one is still ours.
	if (window->fullscreen()) {
		if (m.layout == nullptr || m.target.isNull() || currentFrame(window->hwnd()) != m.target) {
			return State::Suspended;
		}
	}

	return State::Tiled;
}

bool TilingManager::floatsByRule(const Managed& m) const {
	auto* hwnd = m.window->hwnd();

	// Dialogs and other owned windows (tracked when they have WS_EX_APPWINDOW). A hidden owner
	// is a toolkit's application window (Delphi, some Java and Qt apps), not a parent.
	auto* owner = GetWindow(hwnd, GW_OWNER);
	if (owner != nullptr && IsWindowVisible(owner)) return true;

	auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

	constexpr auto FLOATING_EX =
	    WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_DLGMODALFRAME;
	if ((exStyle & FLOATING_EX) != 0) return true;

	// Without a sizing border: fixed size windows, splash screens, popups.
	if ((style & WS_THICKFRAME) == 0) return true;

	if (windowClass(hwnd) == QStringLiteral("#32770")) return true;

	return this->isExcluded(m.window);
}

bool TilingManager::isExcluded(TrackedWindow* window) const {
	for (const auto& pattern: this->excludedTitles) {
		if (pattern.match(window->title()).hasMatch()) return true;
	}

	if (this->excludedPatterns.isEmpty()) return false;

	auto exe = QFileInfo(window->exePath()).fileName();
	auto base = exe;
	if (base.endsWith(".exe", Qt::CaseInsensitive)) base.chop(4);

	auto candidates = QStringList {window->appId(), exe, base, windowClass(window->hwnd())};

	for (const auto& pattern: this->excludedPatterns) {
		for (const auto& candidate: candidates) {
			if (!candidate.isEmpty() && pattern.match(candidate).hasMatch()) return true;
		}
	}

	return false;
}

// --- layouts -----------------------------------------------------------------------------------

tiling::DwindleLayout::Id TilingManager::idOf(TrackedWindow* window) {
	return reinterpret_cast<tiling::DwindleLayout::Id>(window);
}

TrackedWindow* TilingManager::windowOf(tiling::DwindleLayout::Id id) const {
	// Only compared as a key, never dereferenced unless it is still managed.
	auto* window = reinterpret_cast<TrackedWindow*>(id); // NOLINT(performance-no-int-to-ptr)
	return this->managed.contains(window) ? window : nullptr;
}

TilingManager::Layout* TilingManager::layoutFor(TrackedWindow* window, bool create) {
	auto index = window->desktop();
	if (index < 0 || index >= this->desktops->count()) return nullptr;
	return this->layoutAt(this->desktops->desktops().at(index).id, window->screen(), create);
}

TilingManager::Layout* TilingManager::layoutAt(const GUID& desktop, QScreen* screen, bool create) {
	if (screen == nullptr) return nullptr;

	auto key = VirtualDesktops::guidToString(desktop) + '|' + screen->name();
	auto iter = this->layouts.find(key);
	if (iter != this->layouts.end()) return iter->second.get();
	if (!create) return nullptr;

	auto layout = std::make_unique<Layout>();
	layout->desktop = desktop;
	layout->screen = screen;
	layout->tree.setPreserveSplit(this->mPreserveSplit);

	auto* raw = layout.get();
	this->layouts.emplace(key, std::move(layout));
	return raw;
}

bool TilingManager::layoutAlive(const Layout* layout) const {
	if (layout->screen == nullptr) return false;
	if (!QGuiApplication::screens().contains(layout->screen.data())) return false;
	return this->desktops->indexOf(layout->desktop) != -1;
}

void TilingManager::markWindow(TrackedWindow* window) {
	if (!this->mEnabled) return;
	this->dirtyWindows.insert(window);
	this->schedule();
}

void TilingManager::markLayout(Layout* layout) {
	if (!this->mEnabled || layout == nullptr) return;
	this->dirtyLayouts.insert(layout);
	this->schedule();
}

void TilingManager::schedule() {
	if (!this->syncTimer.isActive()) this->syncTimer.start();
}

void TilingManager::sync() {
	if (!this->mEnabled) return;

	// Layouts whose screen or desktop went away hand their windows back for a new home.
	for (auto iter = this->layouts.begin(); iter != this->layouts.end();) {
		auto* layout = iter->second.get();
		if (this->layoutAlive(layout)) {
			++iter;
			continue;
		}

		for (auto id: layout->tree.ids()) {
			if (auto* window = this->windowOf(id)) {
				auto& m = this->managed.at(window);
				m.layout = nullptr;
				m.target = QRect();
				this->dirtyWindows.insert(window);
			}
		}

		this->dirtyLayouts.remove(layout);
		iter = this->layouts.erase(iter);
	}

	auto windows = std::move(this->dirtyWindows);
	this->dirtyWindows.clear();

	// Tracker order (oldest first) keeps insertion deterministic within one batch.
	for (auto* window: this->tracker->windows()) {
		if (windows.contains(window)) this->syncWindow(window);
	}

	this->dropEmptyLayouts();

	auto layouts = std::move(this->dirtyLayouts);
	this->dirtyLayouts.clear();

	for (auto& [key, layout]: this->layouts) {
		if (layouts.contains(layout.get())) this->apply(layout.get());
	}
}

void TilingManager::syncWindow(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	// Decided when the drag ends.
	if (window == this->dragging) return;

	auto state = this->stateOf(m);

	if (state == State::Out) {
		if (m.layout == nullptr) return;

		// Floated (by the user, a rule or because it refused its tile): give it its old size
		// back. Windows that went to every desktop keep where they are.
		auto floated = !window->minimized() && window->desktop() >= 0 && !m.elevated;
		this->takeOut(m);
		if (floated) this->restoreFloating(m);
		return;
	}

	auto index = window->desktop();
	if (index >= this->desktops->count()) return; // a stale index; the refresh marks it again
	auto desktop = this->desktops->desktops().at(index).id;

	// A window keeps its layout while the layout lives and the window stays on its desktop and
	// monitor. Moves to another desktop are always followed, moves to another monitor when they
	// weren't ours (Win+Shift+Arrow, the app itself): while one of our requests is in flight the
	// window may still be on its old monitor.
	Layout* want = nullptr;
	auto* current = m.layout;
	if (current != nullptr && this->layoutAlive(current) && IsEqualGUID(current->desktop, desktop)) {
		want = current;

		if (state == State::Tiled && !m.verifying && window->screen() != current->screen) {
			this->moveToLayout(m, this->layoutFor(window, true), nullptr);
			return;
		}
	} else if (state == State::Tiled) {
		want = this->layoutFor(window, true);
	}

	if (want == m.layout) {
		// Back from minimized, maximized or fullscreen in its slot: a fresh placement, not a
		// correction. A minimized slot gives its space to the others meanwhile.
		if (m.parked && state == State::Tiled) m.target = QRect();
		m.parked = state != State::Tiled;
		this->markLayout(m.layout);
		return;
	}

	if (m.layout != nullptr) this->takeOut(m);

	if (want != nullptr) {
		auto cursor = cursorPos();
		this->insertInto(m, want, nullptr, &cursor);
	}
}

void TilingManager::insertInto(
    Managed& m,
    Layout* layout,
    TrackedWindow* target,
    const QPoint* cursor
) {
	auto* window = m.window;

	if (m.preTiling.isNull()) {
		QRect raw;
		QRect frame;
		if (readFrame(window->hwnd(), raw, frame)) m.preTiling = raw;
	}

	if (target == nullptr || !layout->tree.contains(idOf(target))) target = layout->lastFocused;
	// Not into a minimized window's slot, which would hand the new window its whole space.
	if (target != nullptr && layout->tree.isHidden(idOf(target))) target = nullptr;
	auto targetId = target == nullptr ? tiling::DwindleLayout::Id(0) : idOf(target);

	layout->tree.insert(idOf(window), targetId, cursor);
	m.layout = layout;
	m.parked = false;
	m.target = QRect();
	m.accepted = QRect();
	m.corrections = 0;

	qCDebug(logTiling) << "Tiling" << window->addressHex() << window->appId() << "on"
	                   << VirtualDesktops::guidToString(layout->desktop) << layout->screen->name();

	this->markLayout(layout);
	emit this->windowTilingChanged(window);
}

void TilingManager::takeOut(Managed& m) {
	auto* layout = m.layout;
	if (layout == nullptr) return;

	layout->tree.remove(idOf(m.window));
	if (layout->lastFocused == m.window) layout->lastFocused = nullptr;

	m.layout = nullptr;
	m.target = QRect();
	m.accepted = QRect();
	m.verifying = false;
	this->unverified.remove(m.window);

	this->markLayout(layout);
	emit this->windowTilingChanged(m.window);
}

void TilingManager::dropEmptyLayouts() {
	for (auto iter = this->layouts.begin(); iter != this->layouts.end();) {
		if (iter->second->tree.isEmpty()) {
			this->dirtyLayouts.remove(iter->second.get());
			iter = this->layouts.erase(iter);
		} else {
			++iter;
		}
	}
}

void TilingManager::apply(Layout* layout) {
	if (!this->layoutAlive(layout)) return;

	auto rects = monitorRects(monitorForScreen(layout->screen));
	if (!rects.valid) return;

	layout->tree.setPreserveSplit(this->mPreserveSplit);
	for (auto id: layout->tree.ids()) {
		auto* window = this->windowOf(id);
		auto hidden = window != nullptr && this->stateOf(this->managed.at(window)) == State::Hidden;
		layout->tree.setHidden(id, hidden);
	}

	layout->tree.compute(rects.work);

	for (auto id: layout->tree.ids()) {
		auto* window = this->windowOf(id);
		if (window == nullptr || window == this->dragging) continue;

		auto& m = this->managed.at(window);
		if (this->stateOf(m) != State::Tiled) continue;

		this->place(m, this->tileRect(layout, window));
	}
}

QRect TilingManager::tileRect(const Layout* layout, TrackedWindow* window) const {
	auto box = layout->tree.box(idOf(window));
	auto area = layout->tree.area();
	auto dpr = layout->screen->devicePixelRatio();
	auto in = toPhysical(this->mGapsIn, dpr);
	auto out = toPhysical(this->mGapsOut, dpr);

	// Hyprland's gaps: gaps_out along the work area's edges, gaps_in on every inner side.
	auto left = box.left() == area.left() ? out : in;
	auto top = box.top() == area.top() ? out : in;
	auto right = rightOf(box) == rightOf(area) ? out : in;
	auto bottom = bottomOf(box) == bottomOf(area) ? out : in;

	return QRect(
	    box.left() + left,
	    box.top() + top,
	    qMax(1, box.width() - left - right),
	    qMax(1, box.height() - top - bottom)
	);
}

void TilingManager::place(Managed& m, const QRect& tile) {
	auto frame = currentFrame(m.window->hwnd());

	if (tile != m.target) {
		m.target = tile;
		m.accepted = QRect();
		m.corrections = 0;

		if (frame == tile) m.verifying = false;
		else this->send(m, frame);
		return;
	}

	if (frame == tile) {
		m.verifying = false;
		return;
	}

	if (!m.accepted.isNull() && frame == m.accepted) return;
	// The last request is still on its way; the deadline catches windows that never take it.
	if (m.verifying && frame == m.before) return;

	// It moved, but not onto its tile, or away from it again: the borders changed with the DPI,
	// it has a minimum size or a size step, or the user or the app moved it.
	auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - m.lastCorrection > CORRECTION_RESET_MS) m.corrections = 0;

	if (m.corrections >= MAX_CORRECTIONS) {
		this->giveUp(m, frame);
		return;
	}

	m.corrections++;
	m.lastCorrection = now;
	this->send(m, frame);
}

void TilingManager::send(Managed& m, const QRect& frame) {
	m.before = frame;
	m.verifying = true;
	setFrame(m.window->hwnd(), m.target);
	this->deadlineTimer.start();
}

void TilingManager::restoreFloating(Managed& m) {
	auto* hwnd = m.window->hwnd();
	auto previous = m.preTiling;
	m.preTiling = QRect();

	if (previous.isNull() || IsIconic(hwnd) || IsZoomed(hwnd)) return;

	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return;

	// Back where it was when that is on the monitor it is on now; otherwise its old size,
	// centered where its tile was and kept inside the work area.
	auto rect = previous;
	auto previousRect = toRECT(previous);
	auto* then = MonitorFromRect(&previousRect, MONITOR_DEFAULTTONEAREST);
	auto* now = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

	if (then != now) {
		auto work = monitorRects(now).work;
		rect.moveCenter(raw.center());
		if (work.isValid()) {
			rect.setSize(rect.size().boundedTo(work.size()));
			rect.moveLeft(qBound(work.left(), rect.left(), rightOf(work) - rect.width()));
			rect.moveTop(qBound(work.top(), rect.top(), bottomOf(work) - rect.height()));
		}
	}

	setRawRect(hwnd, rect);
}

// --- events ------------------------------------------------------------------------------------

void TilingManager::onScreensChanged() {
	// Work area changes (the taskbar, AppBars like the bar) arrive as availableGeometryChanged.
	for (auto* screen: QGuiApplication::screens()) {
		QObject::disconnect(screen, nullptr, this, nullptr);
		auto relayoutAll = [this]() {
			for (auto& [key, layout]: this->layouts) this->markLayout(layout.get());
		};
		QObject::connect(screen, &QScreen::availableGeometryChanged, this, relayoutAll);
		QObject::connect(screen, &QScreen::geometryChanged, this, relayoutAll);
		QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, this, relayoutAll);
	}

	this->markAll();
}

void TilingManager::onActiveWindowChanged() {
	auto* window = this->tracker->activeWindow();
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	auto& m = iter->second;
	m.focusStamp = ++this->focusCounter;
	// Only windows already in a layout: a new window is focused before it is inserted, and
	// it should split the window that had focus before it.
	if (m.layout != nullptr) m.layout->lastFocused = window;
}

void TilingManager::onRectChanged(TrackedWindow* window) {
	if (window == this->dragging) return;

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	const auto& m = iter->second;
	if (m.layout == nullptr || m.target.isNull()) return;

	this->unverified.insert(window);
	if (!this->verifyTimer.isActive()) this->verifyTimer.start();
}

void TilingManager::verify() {
	auto pending = std::move(this->unverified);
	this->unverified.clear();

	for (auto* window: pending) {
		auto iter = this->managed.find(window);
		if (iter == this->managed.end() || window == this->dragging) continue;

		auto& m = iter->second;
		if (m.layout == nullptr || m.target.isNull()) continue;
		// Maximized, minimized and the like are handled by the state change.
		if (this->stateOf(m) != State::Tiled) continue;

		this->place(m, m.target);
	}
}

void TilingManager::giveUp(Managed& m, const QRect& frame) {
	m.verifying = false;

	auto bigger = frame.width() > m.target.width() + EDGE_TOLERANCE
	           || frame.height() > m.target.height() + EDGE_TOLERANCE;
	auto away = !m.target.contains(frame.center());

	if (bigger || away) {
		// A minimum size larger than the tile, or an app that keeps putting itself elsewhere.
		qCInfo(logTiling) << "Window" << m.window->addressHex() << m.window->appId()
		                  << "doesn't take its tile" << m.target << "(is" << frame << "); it floats.";
		m.autoFloat = true;
		m.override = Override::None;
		this->markWindow(m.window);
	} else {
		// Close enough: terminals and the like round their size down to whole cells.
		m.accepted = frame;
	}
}

void TilingManager::onDeadline() {
	for (auto& [window, m]: this->managed) {
		if (!m.verifying || m.layout == nullptr || m.target.isNull() || window == this->dragging) {
			continue;
		}

		auto* hwnd = window->hwnd();
		auto frame = currentFrame(hwnd);

		if (frame == m.target) {
			m.verifying = false;
			continue;
		}

		// Moved somewhere: the verification pass deals with it.
		if (frame != m.before) {
			this->unverified.insert(window);
			if (!this->verifyTimer.isActive()) this->verifyTimer.start();
			continue;
		}

		// The request is queued until it responds again.
		if (IsHungAppWindow(hwnd)) continue;

		qCInfo(logTiling) << "Window" << window->addressHex() << window->appId()
		                  << "ignores being moved; it floats.";
		m.verifying = false;
		m.autoFloat = true;
		m.override = Override::None;
		this->markWindow(window);
	}
}

void TilingManager::onMoveSizeStarted(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return;
	if (this->stateOf(iter->second) != State::Tiled) return;

	this->dragging = window;
	this->unverified.remove(window);
}

void TilingManager::onMoveSizeEnded(TrackedWindow* window) {
	if (window != this->dragging) return;
	this->dragging = nullptr;

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	// The tracker's state lags behind the event; ask Windows directly. Dragged to the top edge
	// (maximized) or anything else that changed the state: the state change decides.
	auto* hwnd = window->hwnd();
	if (m.layout == nullptr || m.target.isNull() || IsIconic(hwnd) || IsZoomed(hwnd)) {
		this->markWindow(window);
		return;
	}

	auto frame = currentFrame(hwnd);
	// Where it was before the drag: its tile, or the size it settled on near it.
	const auto target = m.accepted.isNull() ? m.target : m.accepted;

	auto movedLeft = qAbs(frame.left() - target.left()) > EDGE_TOLERANCE;
	auto movedTop = qAbs(frame.top() - target.top()) > EDGE_TOLERANCE;
	auto movedRight = qAbs(rightOf(frame) - rightOf(target)) > EDGE_TOLERANCE;
	auto movedBottom = qAbs(bottomOf(frame) - bottomOf(target)) > EDGE_TOLERANCE;
	auto moved = int(movedLeft) + int(movedTop) + int(movedRight) + int(movedBottom);
	auto resized = qAbs(frame.width() - target.width()) > EDGE_TOLERANCE
	            || qAbs(frame.height() - target.height()) > EDGE_TOLERANCE;

	// Whatever happens next is a placement the user asked for, not a correction.
	m.target = QRect();

	if (moved == 0) {
		// A click on the caption, or a drag that came back: just make it exact again.
		this->markLayout(m.layout);
		return;
	}

	// Every edge moved (also when a monitor with another DPI rescaled it on the way): a move.
	if (!resized || moved == 4) {
		this->drop(m, cursorPos());
		return;
	}

	// A border drag: the dividers on the dragged edges follow, with the gap kept.
	auto* layout = m.layout;
	auto in = toPhysical(this->mGapsIn, layout->screen->devicePixelRatio());
	auto id = idOf(window);

	if (movedLeft) layout->tree.moveEdge(id, Edge::Left, frame.left() - in);
	if (movedRight) layout->tree.moveEdge(id, Edge::Right, rightOf(frame) + in);
	if (movedTop) layout->tree.moveEdge(id, Edge::Top, frame.top() - in);
	if (movedBottom) layout->tree.moveEdge(id, Edge::Bottom, bottomOf(frame) + in);

	this->markLayout(layout);
}

void TilingManager::drop(Managed& m, const QPoint& cursor) {
	auto* from = m.layout;
	auto self = idOf(m.window);

	// Over another tiled window on the same desktop (any monitor): swap with it.
	for (auto& [key, layout]: this->layouts) {
		if (!IsEqualGUID(layout->desktop, from->desktop) || !this->layoutAlive(layout.get())) {
			continue;
		}

		for (auto id: layout->tree.ids()) {
			if (id == self) continue;
			auto* other = this->windowOf(id);
			if (other == nullptr) continue;

			auto& o = this->managed.at(other);
			if (this->stateOf(o) != State::Tiled) continue;

			if (layout->tree.box(id).contains(cursor)) {
				this->swapWindows(m, o);
				return;
			}
		}
	}

	// Over another monitor without a tile under the cursor: into that monitor's layout.
	auto point = POINT {cursor.x(), cursor.y()};
	auto* screen = this->tracker->screenFor(MonitorFromPoint(point, MONITOR_DEFAULTTONULL));

	if (screen != nullptr && screen != from->screen) {
		this->moveToLayout(m, this->layoutAt(from->desktop, screen, true), nullptr);
		return;
	}

	// Anywhere else: back to its tile.
	this->markLayout(from);
}

// --- dispatcher operations ---------------------------------------------------------------------

TilingManager::Managed* TilingManager::tiledActive() {
	if (this->tracker == nullptr) return nullptr;

	auto iter = this->managed.find(this->tracker->activeWindow());
	if (iter == this->managed.end()) return nullptr;

	auto& m = iter->second;
	if (m.layout == nullptr || this->stateOf(m) != State::Tiled) return nullptr;
	return &m;
}

TilingManager::Managed* TilingManager::neighbour(const Managed& from, Edge direction) {
	if (from.layout == nullptr) return nullptr;

	auto source = from.layout->tree.box(idOf(from.window));
	Managed* best = nullptr;
	auto bestDistance = std::numeric_limits<int>::max();

	// Boxes are physical and without gaps, so within a layout neighbours touch; across monitors
	// they are as far apart as the work areas.
	for (auto& [key, layout]: this->layouts) {
		if (!IsEqualGUID(layout->desktop, from.layout->desktop) || !this->layoutAlive(layout.get())) {
			continue;
		}

		for (auto id: layout->tree.ids()) {
			auto* window = this->windowOf(id);
			if (window == nullptr || window == from.window) continue;

			auto& m = this->managed.at(window);
			if (this->stateOf(m) != State::Tiled) continue;

			auto box = layout->tree.box(id);
			auto distance = 0;
			auto overlap = 0;

			switch (direction) {
			case Edge::Left:
				distance = source.left() - rightOf(box);
				overlap = qMin(bottomOf(source), bottomOf(box)) - qMax(source.top(), box.top());
				break;
			case Edge::Right:
				distance = box.left() - rightOf(source);
				overlap = qMin(bottomOf(source), bottomOf(box)) - qMax(source.top(), box.top());
				break;
			case Edge::Top:
				distance = source.top() - bottomOf(box);
				overlap = qMin(rightOf(source), rightOf(box)) - qMax(source.left(), box.left());
				break;
			case Edge::Bottom:
				distance = box.top() - bottomOf(source);
				overlap = qMin(rightOf(source), rightOf(box)) - qMax(source.left(), box.left());
				break;
			}

			if (distance < -EDGE_TOLERANCE || overlap <= 0) continue;

			// Nearest first; among equally near ones (a column of windows next to this one), the
			// one focused last, like Hyprland.
			if (best == nullptr || distance < bestDistance
			    || (distance == bestDistance && m.focusStamp > best->focusStamp))
			{
				best = &m;
				bestDistance = distance;
			}
		}
	}

	return best;
}

QScreen* TilingManager::screenInDirection(QScreen* from, Edge direction) const {
	auto source = physicalMonitor(from);
	QScreen* best = nullptr;
	auto bestDistance = std::numeric_limits<int>::max();

	for (auto* screen: QGuiApplication::screens()) {
		if (screen == from) continue;
		auto rect = physicalMonitor(screen);
		auto distance = 0;
		auto overlap = 0;

		switch (direction) {
		case Edge::Left:
			distance = source.left() - rightOf(rect);
			overlap = qMin(bottomOf(source), bottomOf(rect)) - qMax(source.top(), rect.top());
			break;
		case Edge::Right:
			distance = rect.left() - rightOf(source);
			overlap = qMin(bottomOf(source), bottomOf(rect)) - qMax(source.top(), rect.top());
			break;
		case Edge::Top:
			distance = source.top() - bottomOf(rect);
			overlap = qMin(rightOf(source), rightOf(rect)) - qMax(source.left(), rect.left());
			break;
		case Edge::Bottom:
			distance = rect.top() - bottomOf(source);
			overlap = qMin(rightOf(source), rightOf(rect)) - qMax(source.left(), rect.left());
			break;
		}

		if (distance < 0 || overlap <= 0) continue;
		if (distance < bestDistance) {
			best = screen;
			bestDistance = distance;
		}
	}

	return best;
}

void TilingManager::swapWindows(Managed& a, Managed& b) {
	auto* layoutA = a.layout;
	auto* layoutB = b.layout;
	if (layoutA == nullptr || layoutB == nullptr) return;

	if (layoutA == layoutB) {
		layoutA->tree.swap(idOf(a.window), idOf(b.window));
	} else {
		layoutA->tree.replace(idOf(a.window), idOf(b.window));
		layoutB->tree.replace(idOf(b.window), idOf(a.window));
		a.layout = layoutB;
		b.layout = layoutA;
		if (layoutA->lastFocused == a.window) layoutA->lastFocused = b.window;
		if (layoutB->lastFocused == b.window) layoutB->lastFocused = a.window;
	}

	this->markLayout(layoutA);
	this->markLayout(layoutB);
}

void TilingManager::moveToLayout(Managed& m, Layout* layout, TrackedWindow* target) {
	auto* from = m.layout;
	if (from == nullptr || layout == nullptr || from == layout) return;

	from->tree.remove(idOf(m.window));
	if (from->lastFocused == m.window) from->lastFocused = nullptr;
	this->markLayout(from);

	if (target == nullptr || !layout->tree.contains(idOf(target))) target = layout->lastFocused;
	layout->tree.insert(idOf(m.window), target == nullptr ? 0 : idOf(target));
	layout->lastFocused = m.window;
	m.layout = layout;
	m.target = QRect();
	this->markLayout(layout);
}

bool TilingManager::focusDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	auto* next = this->neighbour(*m, direction);
	if (next == nullptr) return false;

	next->window->activate();
	return true;
}

bool TilingManager::moveDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	auto* next = this->neighbour(*m, direction);

	if (next != nullptr && next->layout == m->layout) {
		this->swapWindows(*m, *next);
	} else if (next != nullptr) {
		// Like Hyprland: onto another monitor the window moves next to the neighbour there.
		this->moveToLayout(*m, next->layout, next->window);
	} else if (auto* screen = this->screenInDirection(m->layout->screen, direction)) {
		this->moveToLayout(*m, this->layoutAt(m->layout->desktop, screen, true), nullptr);
	}

	return true;
}

bool TilingManager::swapDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	if (auto* next = this->neighbour(*m, direction)) this->swapWindows(*m, *next);
	return true;
}

bool TilingManager::toggleSplit(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree.toggleSplit(idOf(window))) this->markLayout(layout);
	return true;
}

bool TilingManager::swapSplit(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree.swapSplit(idOf(window))) this->markLayout(layout);
	return true;
}

bool TilingManager::splitRatio(TrackedWindow* window, double value, bool exact) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree.splitRatio(idOf(window), value, exact)) this->markLayout(layout);
	return true;
}

bool TilingManager::resizeTiled(TrackedWindow* window, qint32 dx, qint32 dy) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	auto dpr = layout->screen->devicePixelRatio();
	if (layout->tree.resize(idOf(window), toPhysical(dx, dpr), toPhysical(dy, dpr))) {
		this->markLayout(layout);
	}

	return true;
}

bool TilingManager::setFloating(TrackedWindow* window, bool floating, bool toggle) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return false;

	auto& m = iter->second;
	if (toggle) floating = m.layout != nullptr;

	if (!floating && m.elevated) {
		qCInfo(logTiling) << "Window" << window->addressHex()
		                  << "belongs to a more privileged process and can't be tiled.";
		return false;
	}

	m.override = floating ? Override::Float : Override::Tile;
	if (!floating) m.autoFloat = false;

	this->markWindow(window);
	return true;
}

void TilingManager::resizeFloating(TrackedWindow* window, qint32 dx, qint32 dy, bool exact) {
	if (window == nullptr || window->screen() == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto dpr = window->screen()->devicePixelRatio();
	auto frame = currentFrame(hwnd);
	auto width = toPhysical(dx, dpr);
	auto height = toPhysical(dy, dpr);
	if (!exact) {
		width += frame.width();
		height += frame.height();
	}

	setFrame(hwnd, QRect(frame.topLeft(), QSize(qMax(1, width), qMax(1, height))));
}

void TilingManager::center(TrackedWindow* window) {
	if (window == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto work = monitorRects(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).work;
	if (!work.isValid()) return;

	auto frame = currentFrame(hwnd);
	frame.moveTo(
	    work.left() + (work.width() - frame.width()) / 2,
	    work.top() + (work.height() - frame.height()) / 2
	);
	setFrame(hwnd, frame);
}

// --- Tiling (QML) ------------------------------------------------------------------------------

Tiling::Tiling(QObject* parent): QObject(parent) {
	auto* manager = TilingManager::instance();
	QObject::connect(manager, &TilingManager::enabledChanged, this, &Tiling::enabledChanged);
	QObject::connect(manager, &TilingManager::gapsInChanged, this, &Tiling::gapsInChanged);
	QObject::connect(manager, &TilingManager::gapsOutChanged, this, &Tiling::gapsOutChanged);
	// clang-format off
	QObject::connect(manager, &TilingManager::preserveSplitChanged, this, &Tiling::preserveSplitChanged);
	// clang-format on
	QObject::connect(manager, &TilingManager::excludedChanged, this, &Tiling::excludedChanged);
}

void Tiling::relayout() { TilingManager::instance()->relayout(); }

bool Tiling::enabled() const { return TilingManager::instance()->enabled(); }
void Tiling::setEnabled(bool enabled) { TilingManager::instance()->setEnabled(enabled); }
qint32 Tiling::gapsIn() const { return TilingManager::instance()->gapsIn(); }
void Tiling::setGapsIn(qint32 gaps) { TilingManager::instance()->setGapsIn(gaps); }
qint32 Tiling::gapsOut() const { return TilingManager::instance()->gapsOut(); }
void Tiling::setGapsOut(qint32 gaps) { TilingManager::instance()->setGapsOut(gaps); }
bool Tiling::preserveSplit() const { return TilingManager::instance()->preserveSplit(); }
void Tiling::setPreserveSplit(bool preserve) {
	TilingManager::instance()->setPreserveSplit(preserve);
}
QStringList Tiling::excluded() const { return TilingManager::instance()->excluded(); }
void Tiling::setExcluded(const QStringList& excluded) {
	TilingManager::instance()->setExcluded(excluded);
}

} // namespace qs::windows
