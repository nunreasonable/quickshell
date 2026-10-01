#pragma once

#include <qcontainerfwd.h>
#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "connection.hpp"
#include "hyprland_toplevel.hpp"

namespace qs::hyprland::ipc {

class HyprlandMonitor;

///! A virtual desktop, presented like a Hyprland workspace.
/// Workspace `id` N is virtual desktop N (1-based); `name` is the desktop's name when the user
/// gave it one, otherwise the id. Desktops are global across monitors, so `active` and
/// `focused` are the same thing and `monitor` is the focused monitor.
class HyprlandWorkspace: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(qint32 id READ default NOTIFY idChanged BINDABLE bindableId);
	Q_PROPERTY(QString name READ default NOTIFY nameChanged BINDABLE bindableName);
	/// If this workspace is the current virtual desktop. See also @@focused.
	Q_PROPERTY(bool active READ default NOTIFY activeChanged BINDABLE bindableActive);
	/// If this workspace is the current virtual desktop. See also @@active.
	Q_PROPERTY(bool focused READ default NOTIFY focusedChanged BINDABLE bindableFocused);
	/// If this workspace has a window that is urgent. Always false on Windows for now.
	Q_PROPERTY(bool urgent READ default NOTIFY urgentChanged BINDABLE bindableUrgent);
	/// If this workspace currently has a fullscreen window.
	Q_PROPERTY(bool hasFullscreen READ default NOTIFY hasFullscreenChanged BINDABLE bindableHasFullscreen);
	/// `hyprctl workspaces -j` style object for this workspace. Kept live on Windows.
	Q_PROPERTY(QVariantMap lastIpcObject READ lastIpcObject NOTIFY lastIpcObjectChanged);
	Q_PROPERTY(qs::hyprland::ipc::HyprlandMonitor* monitor READ default NOTIFY monitorChanged BINDABLE bindableMonitor);
	/// List of toplevels on this workspace.
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::hyprland::ipc::HyprlandToplevel*);
	Q_PROPERTY(UntypedObjectModel* toplevels READ toplevels CONSTANT);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("HyprlandWorkspaces must be retrieved from the HyprlandIpc object.");

public:
	explicit HyprlandWorkspace(HyprlandIpc* ipc);

	void updateInitial(qint32 id, const QString& name);
	void refreshIpcObject();

	/// Switch to the workspace's virtual desktop.
	///
	/// > [!NOTE] This is equivalent to running
	/// > ```qml
	/// > HyprlandIpc.dispatch(`workspace ${workspace.id}`);
	/// > ```
	Q_INVOKABLE void activate();

	[[nodiscard]] QBindable<qint32> bindableId() { return &this->bId; }
	[[nodiscard]] QBindable<QString> bindableName() { return &this->bName; }
	[[nodiscard]] QBindable<bool> bindableActive() { return &this->bActive; }
	[[nodiscard]] QBindable<bool> bindableFocused() { return &this->bFocused; }
	[[nodiscard]] QBindable<bool> bindableUrgent() { return &this->bUrgent; }
	[[nodiscard]] QBindable<bool> bindableHasFullscreen() { return &this->bHasFullscreen; }
	[[nodiscard]] QBindable<HyprlandMonitor*> bindableMonitor() { return &this->bMonitor; }
	[[nodiscard]] ObjectModel<HyprlandToplevel>* toplevels() { return &this->mToplevels; }

	[[nodiscard]] QVariantMap lastIpcObject() const { return this->mLastIpcObject; }

	void insertToplevel(HyprlandToplevel* toplevel);
	void removeToplevel(HyprlandToplevel* toplevel);

signals:
	void idChanged();
	void nameChanged();
	void activeChanged();
	void focusedChanged();
	void urgentChanged();
	void hasFullscreenChanged();
	void lastIpcObjectChanged();
	void monitorChanged();

private slots:
	void updateFullscreen();

private:
	HyprlandIpc* ipc;
	QVariantMap mLastIpcObject;

	ObjectModel<HyprlandToplevel> mToplevels {this};

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(HyprlandWorkspace, qint32, bId, -1, &HyprlandWorkspace::idChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandWorkspace, QString, bName, &HyprlandWorkspace::nameChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandWorkspace, bool, bActive, &HyprlandWorkspace::activeChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandWorkspace, bool, bFocused, &HyprlandWorkspace::focusedChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandWorkspace, bool, bUrgent, &HyprlandWorkspace::urgentChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandWorkspace, bool, bHasFullscreen, &HyprlandWorkspace::hasFullscreenChanged);
	Q_OBJECT_BINDABLE_PROPERTY(HyprlandWorkspace, HyprlandMonitor*, bMonitor, &HyprlandWorkspace::monitorChanged);
	// clang-format on
};

} // namespace qs::hyprland::ipc
