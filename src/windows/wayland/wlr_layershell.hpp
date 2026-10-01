#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../../core/doc.hpp"
#include "../panel_window.hpp"

// Windows stand-in for the wayland WlrLayershell attached object, so configurations written
// for wlr-layer-shell compositors keep working. Enums use the same names and values as the
// wayland module; the properties map onto the Windows panel backend.
namespace qs::wayland::layershell {

///! WlrLayershell layer.
/// See @@WlrLayershell.layer.
namespace WlrLayer { // NOLINT
Q_NAMESPACE;
QML_ELEMENT;

enum Enum : quint8 {
	/// Directly above the desktop, below every other window.
	Background = 0,
	/// A normal window that is not kept on top.
	Bottom = 1,
	/// Always on top of normal windows, lowered while a fullscreen application is active.
	Top = 2,
	/// Always on top, above Top panels and fullscreen applications.
	Overlay = 3,
};
Q_ENUM_NS(Enum);

} // namespace WlrLayer

///! WlrLayershell keyboard focus mode
/// See @@WlrLayershell.keyboardFocus.
namespace WlrKeyboardFocus { // NOLINT
Q_NAMESPACE;
QML_ELEMENT;

enum Enum : quint8 {
	/// The window never becomes the active window (WS_EX_NOACTIVATE).
	None = 0,
	/// The window is forced to the foreground while shown.
	///
	/// > [!WARNING] You **CANNOT** use this to make a secure lock screen.
	Exclusive = 1,
	/// The window is activated when clicked, like any other window.
	OnDemand = 2,
};
Q_ENUM_NS(Enum);

} // namespace WlrKeyboardFocus

///! Layer-shell properties of a PanelWindow (Windows stand-in)
/// Attached object of @@Quickshell.PanelWindow exposing the layer, namespace and keyboard
/// focus mode, with the same interface as the wayland module.
///
/// ```qml
/// PanelWindow {
///   WlrLayershell.layer: WlrLayer.Bottom
/// }
/// ```
class WlrLayershell: public QObject {
	// clang-format off
	Q_OBJECT;
	/// The shell layer the window sits in. Defaults to `WlrLayer.Top`.
	Q_PROPERTY(qs::wayland::layershell::WlrLayer::Enum layer READ layer WRITE setLayer NOTIFY layerChanged);
	/// Identifier of the window for external tools. Stored only, for now.
	Q_PROPERTY(QString namespace READ ns WRITE setNamespace NOTIFY namespaceChanged);
	/// The degree of keyboard focus taken. Defaults to `WlrKeyboardFocus.None`.
	Q_PROPERTY(qs::wayland::layershell::WlrKeyboardFocus::Enum keyboardFocus READ keyboardFocus WRITE setKeyboardFocus NOTIFY keyboardFocusChanged);
	QML_ATTACHED(WlrLayershell);
	QML_NAMED_ELEMENT(WlrLayershell);
	QML_UNCREATABLE("WlrLayershell is only available as an attached object on Windows.");
	// clang-format on

public:
	explicit WlrLayershell(qs::windows::WinPanelWindow* panel);
	Q_DISABLE_COPY_MOVE(WlrLayershell);

	[[nodiscard]] WlrLayer::Enum layer() const;
	void setLayer(WlrLayer::Enum layer);

	[[nodiscard]] QString ns() const;
	void setNamespace(const QString& ns);

	[[nodiscard]] WlrKeyboardFocus::Enum keyboardFocus() const;
	void setKeyboardFocus(WlrKeyboardFocus::Enum focus);

	static WlrLayershell* qmlAttachedProperties(QObject* object);

signals:
	void layerChanged();
	void namespaceChanged();
	void keyboardFocusChanged();

private:
	qs::windows::WinPanelWindow* panel;
};

} // namespace qs::wayland::layershell
