#pragma once

#include <qcontainerfwd.h>
#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../wayland/toplevel.hpp"
#include "../window_tracker.hpp"
#include "connection.hpp"

namespace qs::hyprland::ipc {

//! Hyprland Toplevel
/// Represents a window the way Hyprland's IPC exposes it, backed by the Windows window tracker.
/// Can also be used as an attached object of a @@Quickshell.Wayland.Toplevel, to resolve a
/// handle to a Hyprland toplevel.
class HyprlandToplevel: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_UNCREATABLE("");
	QML_ATTACHED(HyprlandToplevel);
	// clang-format off
	/// Hexadecimal window address (the HWND), without a `0x` prefix like Hyprland's.
	Q_PROPERTY(QString address READ addressStr NOTIFY addressChanged);
	/// The toplevel handle, exposing the Hyprland toplevel.
	/// Null on an attached object until the window is known to the Hyprland module.
	Q_PROPERTY(HyprlandToplevel* handle READ hyprlandHandle NOTIFY hyprlandHandleChanged);
	/// The @@Quickshell.Wayland.Toplevel of the same window.
	Q_PROPERTY(qs::wayland::toplevel_management::Toplevel* wayland READ waylandHandle NOTIFY waylandHandleChanged);
	/// The title of the toplevel
	Q_PROPERTY(QString title READ default NOTIFY titleChanged BINDABLE bindableTitle);
	/// Whether the toplevel is the foreground window
	Q_PROPERTY(bool activated READ default NOTIFY activatedChanged BINDABLE bindableActivated);
	/// Whether the client is urgent or not. Always false on Windows for now.
	Q_PROPERTY(bool urgent READ default NOTIFY urgentChanged BINDABLE bindableUrgent);
	/// `hyprctl clients -j` style object for this window. Kept live on Windows.
	Q_PROPERTY(QVariantMap lastIpcObject READ default BINDABLE bindableLastIpcObject NOTIFY lastIpcObjectChanged);
	/// The current workspace (virtual desktop) of the toplevel. Windows shown on every desktop
	/// follow the current one.
	Q_PROPERTY(qs::hyprland::ipc::HyprlandWorkspace* workspace READ default NOTIFY workspaceChanged BINDABLE bindableWorkspace);
	/// The current monitor of the toplevel (might be null)
	Q_PROPERTY(qs::hyprland::ipc::HyprlandMonitor* monitor READ default NOTIFY monitorChanged BINDABLE bindableMonitor);
	// clang-format on

public:
	/// Created by HyprlandIpc for a tracked window.
	explicit HyprlandToplevel(HyprlandIpc* ipc, qs::windows::TrackedWindow* window);
	/// When attached from a Toplevel
	explicit HyprlandToplevel(HyprlandIpc* ipc, qs::wayland::toplevel_management::Toplevel* toplevel);

	static HyprlandToplevel* qmlAttachedProperties(QObject* object);

	[[nodiscard]] QString addressStr() const { return QString::number(this->mAddress, 16); }
	[[nodiscard]] quint64 address() const { return this->mAddress; }
	[[nodiscard]] qs::windows::TrackedWindow* window() const { return this->mWindow; }

	[[nodiscard]] HyprlandToplevel* hyprlandHandle();
	[[nodiscard]] qs::wayland::toplevel_management::Toplevel* waylandHandle();

	[[nodiscard]] QBindable<QString> bindableTitle() { return &this->bTitle; }
	[[nodiscard]] QBindable<bool> bindableActivated() { return &this->bActivated; }
	[[nodiscard]] QBindable<bool> bindableUrgent() { return &this->bUrgent; }

	[[nodiscard]] QBindable<QVariantMap> bindableLastIpcObject() const {
		return &this->bLastIpcObject;
	};

	[[nodiscard]] QBindable<HyprlandWorkspace*> bindableWorkspace() { return &this->bWorkspace; }
	[[nodiscard]] QBindable<HyprlandMonitor*> bindableMonitor() { return &this->bMonitor; }

	void refreshIpcObject();
	// Drops the toplevel from its workspace's list; used right before it is deleted.
	void leaveWorkspace();

signals:
	void addressChanged();
	QSDOC_HIDE void waylandHandleChanged();
	QSDOC_HIDE void hyprlandHandleChanged();

	void titleChanged();
	void activatedChanged();
	void urgentChanged();
	void workspaceChanged();
	void monitorChanged();
	void lastIpcObjectChanged();

private slots:
	void onWorkspaceChanged();
	void onToplevelAdded(HyprlandToplevel* toplevel);

private:
	void setHyprlandHandle(HyprlandToplevel* handle);
	void bindToHandle();

	quint64 mAddress = 0;
	HyprlandIpc* ipc;

	// Set for toplevels owned by HyprlandIpc; attached objects go through mHyprlandHandle.
	qs::windows::TrackedWindow* mWindow = nullptr;
	qs::wayland::toplevel_management::Toplevel* mWaylandHandle = nullptr;
	HyprlandToplevel* mHyprlandHandle = nullptr;
	HyprlandWorkspace* memberOf = nullptr;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandToplevel, QString, bTitle, &HyprlandToplevel::titleChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandToplevel, bool, bActivated, &HyprlandToplevel::activatedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandToplevel, bool, bUrgent, &HyprlandToplevel::urgentChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandToplevel, HyprlandWorkspace*, bWorkspace, &HyprlandToplevel::workspaceChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandToplevel, HyprlandMonitor*, bMonitor, &HyprlandToplevel::monitorChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandToplevel, QVariantMap, bLastIpcObject, &HyprlandToplevel::lastIpcObjectChanged);
	// clang-format on
};

} // namespace qs::hyprland::ipc
