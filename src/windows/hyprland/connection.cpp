#include "connection.hpp"

#include <qbytearray.h>
#include <qbytearrayview.h>
#include <qcontainerfwd.h>
#include <qguiapplication.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qproperty.h>
#include <qscreen.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../../core/model.hpp"
#include "../../core/qmlscreen.hpp"
#include "../hotkeys.hpp"
#include "../virtual_desktops.hpp"
#include "../window_tracker.hpp"
#include "dispatcher.hpp"
#include "hyprland_toplevel.hpp"
#include "monitor.hpp"
#include "workspace.hpp"

using namespace qs::windows;

namespace qs::hyprland::ipc {

namespace {
Q_LOGGING_CATEGORY(logHyprlandIpc, "quickshell.hyprland.ipc", QtWarningMsg);
Q_LOGGING_CATEGORY(logHyprlandIpcEvents, "quickshell.hyprland.ipc.events", QtWarningMsg);

QByteArray workspaceName(HyprlandWorkspace* workspace) {
	return workspace == nullptr ? QByteArray() : workspace->bindableName().value().toUtf8();
}

QByteArray workspaceId(HyprlandWorkspace* workspace) {
	return workspace == nullptr ? QByteArray("-1")
	                            : QByteArray::number(workspace->bindableId().value());
}

} // namespace

QVector<QString> HyprlandIpcEvent::parse(qint32 argumentCount) const {
	auto args = QVector<QString>();

	for (auto arg: this->parseView(argumentCount)) {
		args.push_back(QString::fromUtf8(arg));
	}

	return args;
}

QVector<QByteArrayView> HyprlandIpcEvent::parseView(qint32 argumentCount) const {
	return HyprlandIpc::parseEventArgs(this->data, static_cast<quint16>(argumentCount));
}

QString HyprlandIpcEvent::nameStr() const { return QString::fromUtf8(this->name); }
QString HyprlandIpcEvent::dataStr() const { return QString::fromUtf8(this->data); }

QVector<QByteArrayView> HyprlandIpc::parseEventArgs(QByteArrayView event, quint16 count) {
	auto args = QVector<QByteArrayView>();

	for (auto i = 0; i < count - 1; i++) {
		auto splitIdx = event.indexOf(',');
		if (splitIdx == -1) break;
		args.push_back(event.sliced(0, splitIdx));
		event = event.sliced(splitIdx + 1);
	}

	if (!event.isEmpty()) {
		args.push_back(event);
	}

	while (args.length() < count) {
		args.push_back(QByteArrayView());
	}

	return args;
}

HyprlandIpc* HyprlandIpc::instance() {
	static HyprlandIpc* instance = nullptr; // NOLINT

	if (instance == nullptr) {
		instance = new HyprlandIpc();
	}

	return instance;
}

HyprlandIpc::HyprlandIpc()
    : mTracker(WindowTracker::instance())
    , mDesktops(VirtualDesktops::instance())
    , dispatcher(new Dispatcher(this)) {
	// Windows move continuously while dragged; one event per burst is plenty for consumers
	// that rebuild their window lists on every event.
	this->geometryTimer.setSingleShot(true);
	this->geometryTimer.setInterval(100);
	QObject::connect(&this->geometryTimer, &QTimer::timeout, this, &HyprlandIpc::emitGeometryEvent);

	// clang-format off
	QObject::connect(this->mTracker, &WindowTracker::windowAdded, this, &HyprlandIpc::onWindowAdded);
	QObject::connect(this->mTracker, &WindowTracker::windowRemoved, this, &HyprlandIpc::onWindowRemoved);
	QObject::connect(this->mTracker, &WindowTracker::activeWindowChanged, this, &HyprlandIpc::onActiveWindowChanged);
	QObject::connect(this->mTracker, &WindowTracker::flushed, this, &HyprlandIpc::onTrackerFlushed);
	QObject::connect(this->mDesktops, &VirtualDesktops::desktopsChanged, this, &HyprlandIpc::onDesktopsChanged);
	QObject::connect(this->mDesktops, &VirtualDesktops::currentChanged, this, &HyprlandIpc::onCurrentDesktopChanged);
	// clang-format on

	// `global` dispatches reach GlobalShortcut objects, like Hyprland's global dispatcher.
	QObject::connect(this, &HyprlandIpc::dispatchGlobal, this, [](const QString& name) {
		qs::windows::hotkeys::HotkeyManager::instance()->triggerGlobal(name);
	});

	if (auto* app = qobject_cast<QGuiApplication*>(QGuiApplication::instance())) {
		QObject::connect(app, &QGuiApplication::screenAdded, this, &HyprlandIpc::onScreensChanged);
		QObject::connect(app, &QGuiApplication::screenRemoved, this, &HyprlandIpc::onScreensChanged);
	}

	this->syncMonitors(true);
	this->syncWorkspaces(true);

	for (auto* window: this->mTracker->windows()) this->onWindowAdded(window);
	this->onActiveWindowChanged();

	qCInfo(logHyprlandIpc) << "Hyprland compatibility module ready:" << this->mMonitors.valueList().length()
	                       << "monitors," << this->mWorkspaces.valueList().length() << "workspaces,"
	                       << this->mToplevels.valueList().length() << "toplevels";

	emit this->connected();
}

void HyprlandIpc::dispatch(const QString& request) { this->dispatcher->dispatch(request); }

ObjectModel<HyprlandMonitor>* HyprlandIpc::monitors() { return &this->mMonitors; }
ObjectModel<HyprlandWorkspace>* HyprlandIpc::workspaces() { return &this->mWorkspaces; }
ObjectModel<HyprlandToplevel>* HyprlandIpc::toplevels() { return &this->mToplevels; }

HyprlandMonitor* HyprlandIpc::monitorForScreen(QScreen* screen) const {
	if (screen == nullptr) return nullptr;

	for (auto* monitor: this->mMonitors.valueList()) {
		if (monitor->screen() == screen) return monitor;
	}

	return nullptr;
}

HyprlandMonitor* HyprlandIpc::monitorFor(QuickshellScreenInfo* screen) {
	if (screen == nullptr) return nullptr;
	if (auto* monitor = this->monitorForScreen(screen->screen)) return monitor;

	for (auto* monitor: this->mMonitors.valueList()) {
		if (monitor->bindableName().value() == screen->name()) return monitor;
	}

	return nullptr;
}

HyprlandWorkspace* HyprlandIpc::workspaceById(qint32 id) const {
	const auto& list = this->mWorkspaces.valueList();
	if (id < 1 || id > list.length()) return nullptr;
	auto* workspace = list[id - 1];
	return workspace->bindableId().value() == id ? workspace : nullptr;
}

HyprlandWorkspace* HyprlandIpc::workspaceByName(const QString& name) const {
	for (auto* workspace: this->mWorkspaces.valueList()) {
		if (workspace->bindableName().value() == name) return workspace;
	}

	return nullptr;
}

HyprlandToplevel* HyprlandIpc::findToplevelByAddress(quint64 address) const {
	for (auto* toplevel: this->mToplevels.valueList()) {
		if (toplevel->address() == address) return toplevel;
	}

	return nullptr;
}

HyprlandToplevel* HyprlandIpc::toplevelForWindow(TrackedWindow* window) const {
	return window == nullptr ? nullptr : this->byWindow.value(window);
}

void HyprlandIpc::refreshMonitors() {
	for (auto* monitor: this->mMonitors.valueList()) monitor->refreshIpcObject();
}

void HyprlandIpc::refreshWorkspaces() {
	for (auto* workspace: this->mWorkspaces.valueList()) workspace->refreshIpcObject();
}

void HyprlandIpc::refreshToplevels() {
	for (auto* toplevel: this->mToplevels.valueList()) toplevel->refreshIpcObject();
}

void HyprlandIpc::emitEvent(const QByteArray& name, const QByteArray& data) {
	qCDebug(logHyprlandIpcEvents) << "Event" << name << data;
	this->event.name = name;
	this->event.data = data;
	emit this->rawEvent(&this->event);
}

void HyprlandIpc::emitGeometryEvent() { this->emitEvent("windowgeometry", ""); }

void HyprlandIpc::syncMonitors(bool initial) {
	auto screens = QGuiApplication::screens();
	QList<HyprlandMonitor*> ordered;
	QList<HyprlandMonitor*> added;

	for (qsizetype i = 0; i < screens.length(); i++) {
		auto* monitor = this->monitorForScreen(screens[i]);

		if (monitor == nullptr) {
			monitor = new HyprlandMonitor(this, screens[i]);
			added.append(monitor);
		}

		monitor->updateFromScreen(static_cast<qint32>(i));
		ordered.append(monitor);
	}

	QList<HyprlandMonitor*> removed;
	for (auto* monitor: this->mMonitors.valueList()) {
		if (!ordered.contains(monitor)) removed.append(monitor);
	}

	this->mMonitors.diffUpdate(ordered);
	this->bMonitorsVersion = this->bMonitorsVersion.value() + 1;

	for (auto* monitor: removed) {
		if (this->bFocusedMonitor.value() == monitor) this->bFocusedMonitor = nullptr;
		if (!initial) this->emitEvent("monitorremoved", monitor->bindableName().value().toUtf8());
		// like upstream: keep the object around for a cycle in case something still references it
		monitor->deleteLater();
	}

	for (auto* monitor: added) {
		if (initial) continue;
		auto name = monitor->bindableName().value().toUtf8();
		this->emitEvent(
		    "monitoraddedv2",
		    QByteArray::number(monitor->bindableId().value()) + "," + name + ","
		        + monitor->bindableDescription().value().toUtf8()
		);
		this->emitEvent("monitoradded", name);
	}

	if (this->bFocusedMonitor.value() == nullptr && !ordered.isEmpty()) {
		auto* primary = this->monitorForScreen(QGuiApplication::primaryScreen());
		this->updateFocusedMonitor(primary == nullptr ? ordered.first() : primary);
	}
}

void HyprlandIpc::syncWorkspaces(bool initial) {
	const auto& desktops = this->mDesktops->desktops();
	auto count = desktops.length();
	const auto& list = this->mWorkspaces.valueList();

	auto nameFor = [&desktops](qsizetype index) {
		const auto& name = desktops[index].name;
		return name.isEmpty() ? QString::number(index + 1) : name;
	};

	// Workspace ids are positions, so a desktop removed in the middle shows up as the last
	// workspace going away and the others being renamed; windows are re-queried by the tracker.
	while (list.length() > count) {
		auto* workspace = list.last();
		auto id = workspaceId(workspace);
		auto name = workspaceName(workspace);

		if (this->bFocusedWorkspace.value() == workspace) this->bFocusedWorkspace = nullptr;
		this->mWorkspaces.removeAt(list.length() - 1);

		if (!initial) {
			this->emitEvent("destroyworkspacev2", id + "," + name);
			this->emitEvent("destroyworkspace", name);
		}

		workspace->deleteLater();
	}

	for (qsizetype i = 0; i < list.length(); i++) {
		auto* workspace = list[i];
		auto name = nameFor(i);

		if (workspace->bindableName().value() != name) {
			workspace->bindableName().setValue(name);
			if (!initial) this->emitEvent("renameworkspace", workspaceId(workspace) + "," + name.toUtf8());
		}
	}

	for (auto i = list.length(); i < count; i++) {
		auto* workspace = new HyprlandWorkspace(this);
		workspace->updateInitial(static_cast<qint32>(i + 1), nameFor(i));
		this->mWorkspaces.insertObject(workspace);

		if (!initial) {
			this->emitEvent("createworkspacev2", workspaceId(workspace) + "," + workspaceName(workspace));
			this->emitEvent("createworkspace", workspaceName(workspace));
		}
	}

	this->bWorkspacesVersion = this->bWorkspacesVersion.value() + 1;
	this->updateFocusedWorkspace();
}

void HyprlandIpc::updateFocusedWorkspace() {
	const auto& list = this->mWorkspaces.valueList();
	auto index = this->mDesktops->currentIndex();
	auto* workspace = index >= 0 && index < list.length() ? list[index] : nullptr;
	this->bFocusedWorkspace = workspace;
}

void HyprlandIpc::updateFocusedMonitor(HyprlandMonitor* monitor) {
	if (monitor == nullptr || monitor == this->bFocusedMonitor.value()) return;
	this->bFocusedMonitor = monitor;

	this->emitEvent(
	    "focusedmon",
	    monitor->bindableName().value().toUtf8() + "," + workspaceName(this->bFocusedWorkspace.value())
	);
}

void HyprlandIpc::onWindowAdded(TrackedWindow* window) {
	if (this->byWindow.contains(window)) return;

	auto* toplevel = new HyprlandToplevel(this, window);
	this->byWindow.insert(window, toplevel);
	this->mToplevels.insertObject(toplevel);

	auto address = toplevel->addressStr().toUtf8();

	QObject::connect(window, &TrackedWindow::titleChanged, this, [this, window, address]() {
		this->emitEvent("windowtitlev2", address + "," + window->title().toUtf8());
		this->emitEvent("windowtitle", address);
	});

	QObject::connect(toplevel, &HyprlandToplevel::workspaceChanged, this, [this, toplevel, address]() {
		auto* workspace = toplevel->bindableWorkspace().value();
		if (workspace == nullptr) return;
		this->emitEvent("movewindowv2", address + "," + workspaceId(workspace) + "," + workspaceName(workspace));
		this->emitEvent("movewindow", address + "," + workspaceName(workspace));
	});

	QObject::connect(window, &TrackedWindow::fullscreenChanged, this, [this, window]() {
		this->emitEvent("fullscreen", window->fullscreen() ? "1" : "0");
	});

	auto geometry = [this]() {
		if (!this->geometryTimer.isActive()) this->geometryTimer.start();
	};

	QObject::connect(window, &TrackedWindow::rectChanged, this, geometry);
	QObject::connect(window, &TrackedWindow::minimizedChanged, this, geometry);
	QObject::connect(window, &TrackedWindow::maximizedChanged, this, geometry);
	QObject::connect(window, &TrackedWindow::screenChanged, this, geometry);

	this->emitEvent(
	    "openwindow",
	    address + "," + workspaceName(toplevel->bindableWorkspace().value()) + ","
	        + window->appId().toUtf8() + "," + window->title().toUtf8()
	);

	emit this->toplevelAdded(toplevel);
}

void HyprlandIpc::onWindowRemoved(TrackedWindow* window) {
	auto* toplevel = this->byWindow.take(window);
	if (toplevel == nullptr) return;

	if (this->bActiveToplevel.value() == toplevel) this->bActiveToplevel = nullptr;
	this->mToplevels.removeObject(toplevel);
	toplevel->leaveWorkspace();

	this->emitEvent("closewindow", toplevel->addressStr().toUtf8());
	toplevel->deleteLater();
}

void HyprlandIpc::onActiveWindowChanged() {
	auto* toplevel = this->toplevelForWindow(this->mTracker->activeWindow());
	if (toplevel == this->bActiveToplevel.value() && toplevel != nullptr) return;

	this->bActiveToplevel = toplevel;

	if (toplevel == nullptr) {
		this->emitEvent("activewindowv2", "");
		this->emitEvent("activewindow", ",");
		return;
	}

	this->updateFocusedMonitor(toplevel->bindableMonitor().value());
	this->emitEvent("activewindowv2", toplevel->addressStr().toUtf8());
	this->emitEvent(
	    "activewindow",
	    toplevel->window()->appId().toUtf8() + "," + toplevel->window()->title().toUtf8()
	);
}

void HyprlandIpc::onTrackerFlushed() {
	// The active window may have moved to another screen.
	if (auto* toplevel = this->bActiveToplevel.value()) {
		this->updateFocusedMonitor(toplevel->bindableMonitor().value());
	}
}

void HyprlandIpc::onDesktopsChanged() { this->syncWorkspaces(false); }

void HyprlandIpc::onCurrentDesktopChanged() {
	auto* previous = this->bFocusedWorkspace.value();
	this->updateFocusedWorkspace();
	auto* workspace = this->bFocusedWorkspace.value();
	if (workspace == nullptr || workspace == previous) return;

	this->emitEvent("workspacev2", workspaceId(workspace) + "," + workspaceName(workspace));
	this->emitEvent("workspace", workspaceName(workspace));

	if (auto* monitor = this->bFocusedMonitor.value()) {
		this->emitEvent(
		    "focusedmon",
		    monitor->bindableName().value().toUtf8() + "," + workspaceName(workspace)
		);
	}
}

void HyprlandIpc::onScreensChanged() { this->syncMonitors(false); }

} // namespace qs::hyprland::ipc
