#pragma once

#include <qt_windows.h>

#include <qcontainerfwd.h>
#include <qfilesystemwatcher.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlengine.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qvariant.h>

#include "keyboard_hook.hpp"

namespace qs::windows::hotkeys {

// Parses "Ctrl+Alt+Shift+Super+X" style key descriptions (case insensitive, Hyprland and XF86
// key names accepted). A single modifier alone ("Super") is a lone modifier combo.
[[nodiscard]] bool parseKeys(const QString& text, KeyCombo& combo, QString& error);
[[nodiscard]] uint8_t keyNameToVk(const QString& name);

struct Bind {
	enum class Action : quint8 {
		// Signals GlobalShortcut objects with a matching appid and name.
		Global,
		// Hyprland.dispatch(argument) of the Quickshell.Hyprland module.
		Dispatch,
		// Starts `argument` detached. %SHELL% is the config dir, %VAR% environment variables.
		Exec,
		// Calls an IpcHandler function in process.
		Ipc,
		// Injects a key press (e.g. a media key) into the system.
		Key,
		// Hard reload of the shell config.
		Reload,
		// Documents a shortcut Windows itself handles; never registered.
		Native,
	};

	enum class Mechanism : quint8 {
		None,
		Hotkey,
		Hook,
		Native,
		Unavailable,
	};

	QString keys;
	KeyCombo combo;
	Action action = Action::Native;
	QString appid;
	QString name;
	QString argument;
	QString function;
	QStringList args;
	uint8_t sendVk = 0;
	bool onRelease = false;
	bool repeat = false;
	QString description;
	Mechanism mechanism = Mechanism::None;
};

// Process wide owner of every global shortcut: loads keybinds.json, registers its binds with
// RegisterHotKey or the low level keyboard hook, and runs their actions on the gui thread.
//
// Binds without the Windows key use RegisterHotKey, which keeps working while an elevated window
// is focused and lets the shell take the foreground. Binds with the Windows key and lone
// modifiers go through the hook: the shell owns most Win+key combos, so RegisterHotKey either
// fails for them or loses to the shell's own handling.
class HotkeyManager: public QObject {
	Q_OBJECT;

public:
	~HotkeyManager() override;
	Q_DISABLE_COPY_MOVE(HotkeyManager);

	static HotkeyManager* instance();

	// The config dir, for %SHELL% and the default keybinds file. Loads on first call.
	void setShellDir(const QString& shellDir);
	void reload();

	void registerShortcut(QObject* shortcut);
	void unregisterShortcut(QObject* shortcut);

	[[nodiscard]] QString configPath() const { return this->mConfigPath; }
	[[nodiscard]] QVariantList bindsInfo() const;
	[[nodiscard]] bool hookActive() const;

signals:
	void bindsChanged();

private:
	explicit HotkeyManager(QObject* parent);

	enum class Phase : quint8 {
		Press,
		Repeat,
		Release,
		Tap,
	};

	struct Trigger {
		HookTrigger::Kind kind = HookTrigger::Combo;
		KeyCombo combo;
		Bind::Mechanism mechanism = Bind::Mechanism::None;
		QList<qsizetype> binds;
		bool repeat = false;
		bool down = false;
	};

	void loadFile();
	void parse(const QByteArray& data, const QString& path);
	void unregisterAll();
	void registerAll();
	void updateWatches();
	void scheduleReload();

	void onHotkey(int id);
	void onHookEvent(HookEvent event, quint32 triggerId, quint32 serial);
	void pollReleases();
	void fire(qsizetype trigger, Phase phase);
	void fireBind(const Bind& bind, Phase phase);
	void emitGlobal(const QString& appid, const QString& name, bool pressed);
	void releaseAllGlobals();

	void runDispatch(const Bind& bind);
	void runExec(const Bind& bind) const;
	void runIpc(const Bind& bind);
	void runReload();

	[[nodiscard]] QString userFilePath() const;
	[[nodiscard]] QString defaultFilePath() const;

	static LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	HWND messageWindow = nullptr;
	QString shellDir;
	QString mConfigPath;
	QByteArray loadedData;
	bool loaded = false;
	bool parsed = false;

	QList<Bind> binds;
	QList<Trigger> triggers;
	quint32 serial = 0;
	bool hookRunning = false;

	QList<QPointer<QObject>> shortcuts;
	QList<QPair<QString, QString>> pressedGlobals;

	QFileSystemWatcher watcher;
	QTimer reloadTimer;
	QTimer releaseTimer;
};

///! Global shortcuts from keybinds.json.
/// Loads `%LOCALAPPDATA%\illogical-impulse\keybinds.json`, or `defaults/windows/keybinds.json`
/// in the config dir if the user has none, and reloads when the user file changes.
///
/// Each bind has `keys` ("Ctrl+Alt+Shift+Super+X", or a lone modifier like "Super"), an `action`
/// (`global`, `dispatch`, `exec`, `ipc`, `key`, `reload` or `native`) with its arguments, and
/// optional `onRelease`, `repeat` and `description`.
///
/// GlobalShortcut objects register here and receive `global` binds by appid and name.
class Hotkeys: public QObject {
	Q_OBJECT;
	/// The keybinds file in use.
	Q_PROPERTY(QString configPath READ configPath NOTIFY bindsChanged);
	/// Every parsed bind with how it was registered (`mechanism`: hotkey, hook, native or
	/// unavailable), for debugging.
	Q_PROPERTY(QVariantList binds READ binds NOTIFY bindsChanged);
	/// If the low level keyboard hook is installed.
	Q_PROPERTY(bool hookActive READ hookActive NOTIFY bindsChanged);
	QML_ELEMENT;
	QML_SINGLETON;

public:
	// Not default constructible on purpose: QML prefers a default constructor over create(),
	// which is where the config dir comes from.
	explicit Hotkeys(QObject* parent);

	static Hotkeys* create(QQmlEngine* engine, QJSEngine* jsEngine);

	[[nodiscard]] QString configPath() const;
	[[nodiscard]] QVariantList binds() const;
	[[nodiscard]] bool hookActive() const;

	/// Delivers `global` binds to `shortcut` (an object with appid and name properties and
	/// pressed() / released() signals) until it is unregistered or destroyed.
	Q_INVOKABLE void registerShortcut(QObject* shortcut);
	Q_INVOKABLE void unregisterShortcut(QObject* shortcut);
	/// Reloads the keybinds file.
	Q_INVOKABLE void reload();

signals:
	void bindsChanged();
};

} // namespace qs::windows::hotkeys
