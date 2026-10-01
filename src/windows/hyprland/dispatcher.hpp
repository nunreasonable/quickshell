#pragma once

#include <qhash.h>
#include <qset.h>
#include <qstring.h>
#include <qtypes.h>

#include "../window_tracker.hpp"

namespace qs::hyprland::ipc {

class HyprlandIpc;

// Executes Hyprland dispatchers on Windows, in the classic spelling (`workspace 2`,
// `movetoworkspace 3,address:0x1234`) and the Lua one configurations written against
// Hyprland's Lua config use (`hl.dsp.focus({workspace = 2})`). Dispatchers without a Windows
// equivalent are logged once and ignored.
class Dispatcher {
public:
	explicit Dispatcher(HyprlandIpc* ipc): ipc(ipc) {}

	void dispatch(const QString& request);

private:
	struct LuaCall {
		QString function;
		QString scalar;                // single argument, unquoted: hl.dsp.global("x")
		QHash<QString, QString> table; // flat table fields, unquoted
		bool ok = false;
	};

	static LuaCall parseLua(const QString& request);
	static QString unquote(QString value);

	void dispatchLua(const LuaCall& call);
	void dispatchClassic(const QString& name, const QString& args);

	// Desktop index for a workspace argument (N, +N, -N, e+N, r+N, name:x); -1 if it has no
	// Windows meaning. `create` is set for absolute ids beyond the existing desktops.
	[[nodiscard]] qsizetype resolveWorkspace(const QString& arg, bool& create) const;
	[[nodiscard]] qs::windows::TrackedWindow* resolveWindow(const QString& arg) const;

	void focusWorkspace(const QString& arg);
	void moveToWorkspace(qs::windows::TrackedWindow* window, const QString& arg, bool follow);
	void fullscreen(qs::windows::TrackedWindow* window, int mode);
	void pin(qs::windows::TrackedWindow* window);
	void moveFocus(const QString& direction);
	void moveWindow(const QString& direction);
	void moveWindowPixel(qs::windows::TrackedWindow* window, const QString& x, const QString& y);
	void exec(const QString& command);

	void unknown(const QString& what);
	void unsupported(const QString& what);

	HyprlandIpc* ipc;
	QSet<QString> warned;
};

} // namespace qs::hyprland::ipc
