#pragma once

#include <memory>
#include <optional>

#include <qt_windows.h>

#include <qbytearray.h>
#include <qfilesystemwatcher.h>
#include <qlist.h>
#include <qmetaobject.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlengine.h>
#include <qqmlintegration.h>
#include <qquickwindow.h>
#include <qrect.h>
#include <qregion.h>
#include <qregularexpression.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qvariant.h>

#include "blur_shapes.hpp"

class QWinEventNotifier;

namespace qs::windows {

class WinPanelWindow;

// Blur behind panels: the Windows counterpart of Hyprland's `blur` and `ignore_alpha` layer
// rules.
//
// A panel whose namespace matches a blur rule gets a backdrop window: a borderless,
// click-through, never activated tool window without a redirection surface, at the panel's
// position and size, kept directly below the panel in the z-order. Its only content is a
// Windows.UI.Composition host backdrop brush (DWMWA_USE_HOSTBACKDROPBRUSH, documented since
// Windows 11 22000), which DWM fills with the blurred desktop behind the window, clipped to
// anti-aliased rounded rectangles.
//
// Windows 10 has no host backdrop for desktop windows. There the same kind of window is blurred
// by the undocumented but long-standing SetWindowCompositionAttribute accent policy, which blurs
// behind the whole window rectangle and ignores the window region: each shape gets a window of
// its own at its rectangle, so rounded corners leave small square patches of blur outside them.
//
// The rectangles come from the panel's item tree instead of its pixels (blur_shapes.hpp): the
// Rectangle items more opaque than the rule's ignoreAlpha, but not fully opaque, so panels
// without transparency cost nothing. Shapes are recomputed on each frame the panel renders: an
// idle panel costs nothing, an animating one a walk of its item tree per frame.

struct BlurRule {
	bool blur = false;
	// Unset: blur behind the whole surface (the input mask if set, else the window).
	std::optional<qreal> ignoreAlpha;
};

class BackdropWindow;
class PanelBlur;

// Process wide: the rules file, the system settings that turn blur off, and the compositor.
class BlurManager: public QObject {
	Q_OBJECT;

public:
	~BlurManager() override;
	Q_DISABLE_COPY_MOVE(BlurManager);

	static BlurManager* instance();

	// The config dir, for the default rules file. Loads on first call.
	void setShellDir(const QString& shellDir);
	void reload();

	[[nodiscard]] BlurRule ruleFor(const QString& ns) const;

	// Runtime switch (BackdropBlur.enabled), on by default.
	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);

	// If blur can show at all right now: supported by this Windows build, Transparency effects
	// on, no high contrast theme, no energy saver, not turned off by QS_WINDOWS_BLUR=0 or the
	// rules file, and not shutting down.
	[[nodiscard]] bool available() const;

	[[nodiscard]] QString configPath() const { return this->mConfigPath; }
	[[nodiscard]] QVariantList rulesInfo() const;

	// How backdrop windows get their blur, decided once from the Windows build.
	enum class Backend : quint8 {
		// Windows 11: a composition tree of host backdrop sprites.
		HostBackdrop,
		// Windows 10: SetWindowCompositionAttribute blur behind, one window per shape.
		Accent,
	};

	[[nodiscard]] Backend backend() const { return this->mBackend; }

	// Backdrops are not layered by default: DWM backdrop materials are reported not to render on
	// layered windows, while WS_EX_TRANSPARENT is reported to make a window without a redirection
	// surface click-through on its own (and a window region limited to the blurred rectangles
	// backs that up). QS_WINDOWS_BLUR_LAYERED=1 or `"layered": true` in the rules file adds
	// WS_EX_LAYERED (and drops the region) in case clicks still get stuck on a backdrop. Accent
	// backdrops are a window per shape, each keeping its rounded region either way.
	[[nodiscard]] bool layeredBackdrops() const;
	// QS_WINDOWS_BLUR_DEBUG=1: tints every blurred shape red, to tell shape placement apart from
	// the backdrop brush rendering.
	[[nodiscard]] bool debugTint() const { return this->mDebugTint; }

	// Re-reads the system settings that turn blur off (coalesced). With `recreate`, every
	// backdrop is rebuilt as well (DWM restarted).
	void scheduleSystemCheck(bool recreate = false);

	// Composition objects are released at aboutToQuit, while COM is still initialized.
	[[nodiscard]] bool shutDown() const { return this->mShutDown; }

	// Windows.UI.Composition state (blur.cpp), created lazily on the gui thread.
	struct Composition;
	// Null when unavailable, after which blur stays off.
	[[nodiscard]] Composition* ensureComposition();
	void markUnsupported(const QString& reason);

	void registerPanel(PanelBlur* panel);
	void unregisterPanel(PanelBlur* panel);

signals:
	void rulesChanged();
	void stateChanged();

private:
	explicit BlurManager(QObject* parent);

	struct Rule {
		QString pattern;
		QRegularExpression regex;
		std::optional<bool> blur;
		// set to an empty optional: explicitly back to "whole surface"
		std::optional<std::optional<qreal>> ignoreAlpha;
	};

	void detectBackend();
	void loadFile();
	void parse(const QByteArray& data, const QString& path);
	void updateWatches();
	void scheduleReload();
	void checkSystem();
	void watchRegistry();
	void armRegistryNotification();
	void notifyPanels();
	void scheduleNotifyPanels();
	void shutdown();
	void closeCompositor();

	[[nodiscard]] QString userFilePath() const;
	[[nodiscard]] QString defaultFilePath() const;

	QString shellDir;
	QString mConfigPath;
	QByteArray loadedData;
	bool loaded = false;
	bool parsed = false;
	QList<Rule> rules;
	bool fileEnabled = true;
	bool fileLayered = false;
	bool envEnabled = true;
	bool envLayered = false;
	bool mDebugTint = false;
	bool mEnabled = true;
	Backend mBackend = Backend::HostBackdrop;

	bool transparencyEffects = true;
	bool highContrast = false;
	bool energySaver = false;
	bool unsupported = false;
	bool systemCheckPending = false;
	bool recreatePending = false;
	bool notifyPending = false;
	bool mShutDown = false;

	QFileSystemWatcher watcher;
	QTimer reloadTimer;

	HKEY personalizeKey = nullptr;
	HANDLE registryEvent = nullptr;
	QWinEventNotifier* registryNotifier = nullptr;

	std::unique_ptr<Composition> composition;

	QList<PanelBlur*> panels;
};

// Blur state of one panel, owned by its WinPanelWindow. Survives reloads through adopt(), like
// the AppBar registration, so the backdrop doesn't flicker when the config reloads.
class PanelBlur: public QObject {
	Q_OBJECT;

public:
	explicit PanelBlur(WinPanelWindow* panel);
	~PanelBlur() override;
	Q_DISABLE_COPY_MOVE(PanelBlur);

	// Takes over the backdrop of the panel this one replaces on reload.
	void adopt(PanelBlur* other);
	// The panel's HWND exists (connected, created or reused).
	void attach();
	// The panel lost its backing window (disowned or destroyed): drops the backdrop.
	void release();

	// The resolved input mask, as applied to the panel (window coordinates).
	void setInputMask(const QRegion& region, bool hasMask);

	// The panel moved, was resized, restacked, shown or hidden (WM_WINDOWPOSCHANGED).
	void syncPlacement();
	// Drops the native backdrop so the next updateActive() builds a new one (DWM restarted, or
	// the window styles changed).
	void recreate();

	// Re-evaluates the rule and the system state.
	void updateActive();

private slots:
	void onFrame();
	void updateShapes();

private:
	void connectFrames();
	void disconnectFrames();
	void scheduleShapes();
	void collectShapes(QList<BlurShape>& shapes);
	bool ensureBackdrop();
	void destroyBackdrop();
	[[nodiscard]] bool panelShown() const;

	WinPanelWindow* panel;
	QPointer<QQuickWindow> mWindow;
	QMetaObject::Connection frameConnection;
	QPointer<QQuickWindow> framesWindow;
	std::unique_ptr<BackdropWindow> backdrop;
	BlurRule rule;
	bool active = false;
	bool shapesPending = false;
	// shapes are from before the panel was last hidden
	bool stale = true;
	bool panelWasShown = false;
	QRegion mask;
	bool hasMask = false;
	QList<BlurShape> shapes;
	bool truncatedWarned = false;
};

///! Blur behind panels on Windows.
/// Blur behind panels is configured by `defaults/windows/layerrules.json` in the config dir, or
/// `%LOCALAPPDATA%\illogical-impulse\layerrules.json` when the user has one. It mirrors
/// Hyprland's layer rules: each entry of `rules` has a `namespace` regex (matched against the
/// whole `WlrLayershell.namespace`) and optional `blur` (bool) and `ignoreAlpha` (0-1, or null to
/// blur behind the whole surface). Later matching rules override earlier ones.
///
/// Blur shows behind the Rectangle items of a panel whose fill (opacity included) is more opaque
/// than `ignoreAlpha` and not fully opaque. It is off while the system's Transparency effects
/// setting is off, or a high contrast theme or energy saver is on.
class BackdropBlur: public QObject {
	Q_OBJECT;
	/// Turns blur behind panels on or off for this session. Defaults to true.
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY stateChanged);
	/// If blur can show: supported by Windows, Transparency effects on, no high contrast or
	/// energy saver.
	Q_PROPERTY(bool available READ available NOTIFY stateChanged);
	/// The rules file in use.
	Q_PROPERTY(QString configPath READ configPath NOTIFY rulesChanged);
	/// The parsed rules, for debugging.
	Q_PROPERTY(QVariantList rules READ rules NOTIFY rulesChanged);
	QML_ELEMENT;
	QML_SINGLETON;

public:
	// Not default constructible on purpose: QML prefers a default constructor over create(),
	// which is where the config dir comes from.
	explicit BackdropBlur(QObject* parent);

	static BackdropBlur* create(QQmlEngine* engine, QJSEngine* jsEngine);

	[[nodiscard]] static bool enabled();
	static void setEnabled(bool enabled);
	[[nodiscard]] static bool available();
	[[nodiscard]] static QString configPath();
	[[nodiscard]] static QVariantList rules();

	/// Reloads the rules file. It also reloads by itself when the file changes.
	Q_INVOKABLE static void reload();

signals:
	void stateChanged();
	void rulesChanged();
};

} // namespace qs::windows
