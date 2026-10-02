#include "dispatcher.hpp"
#include <limits>
#include <string>

#include <qdir.h>
#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpoint.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtypes.h>

#include "../virtual_desktops.hpp"
#include "../window_tracker.hpp"
#include "connection.hpp"
#include "workspace.hpp"

// last: pulls in the rpc headers, which define macros like `small`
#include <shellapi.h>

using namespace qs::windows;

namespace qs::hyprland::ipc {

namespace {
Q_LOGGING_CATEGORY(logDispatch, "quickshell.hyprland.dispatch", QtWarningMsg);

void sendWinShortcut(WORD key) {
	INPUT inputs[4] {};
	inputs[0].type = INPUT_KEYBOARD;
	inputs[0].ki.wVk = VK_LWIN;
	inputs[1].type = INPUT_KEYBOARD;
	inputs[1].ki.wVk = key;
	inputs[2].type = INPUT_KEYBOARD;
	inputs[2].ki.wVk = key;
	inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
	inputs[3].type = INPUT_KEYBOARD;
	inputs[3].ki.wVk = VK_LWIN;
	inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(4, inputs, sizeof(INPUT));
}

WORD arrowKey(const QString& direction) {
	if (direction.startsWith('l')) return VK_LEFT;
	if (direction.startsWith('r')) return VK_RIGHT;
	if (direction.startsWith('u')) return VK_UP;
	if (direction.startsWith('d')) return VK_DOWN;
	return 0;
}

} // namespace

void Dispatcher::dispatch(const QString& request) {
	auto trimmed = request.trimmed();
	if (trimmed.isEmpty()) return;

	qCDebug(logDispatch) << "dispatch" << trimmed;

	if (trimmed.startsWith("hl.")) {
		auto call = Dispatcher::parseLua(trimmed);
		if (!call.ok) {
			qCWarning(logDispatch) << "Could not parse dispatch request" << trimmed;
			return;
		}

		this->dispatchLua(call);
		return;
	}

	auto space = trimmed.indexOf(' ');
	auto name = space == -1 ? trimmed : trimmed.left(space);
	auto args = space == -1 ? QString() : trimmed.mid(space + 1).trimmed();
	this->dispatchClassic(name, args);
}

QString Dispatcher::unquote(QString value) {
	value = value.trimmed();
	if (value.length() >= 2) {
		auto first = value.front();
		if ((first == '"' || first == '\'') && value.back() == first) {
			return value.mid(1, value.length() - 2);
		}
	}

	return value;
}

Dispatcher::LuaCall Dispatcher::parseLua(const QString& request) {
	LuaCall call;

	auto open = request.indexOf('(');
	auto close = request.lastIndexOf(')');
	if (open == -1 || close == -1 || close < open) return call;

	call.function = request.left(open).trimmed();
	call.ok = true;

	auto inner = request.mid(open + 1, close - open - 1).trimmed();
	if (inner.isEmpty()) return call;

	if (!inner.startsWith('{')) {
		call.scalar = Dispatcher::unquote(inner);
		return call;
	}

	auto end = inner.lastIndexOf('}');
	auto body = inner.mid(1, end - 1);

	// Split on top level commas only: values may be quoted or nested tables.
	QStringList fields;
	qsizetype start = 0;
	auto depth = 0;
	auto quote = QChar();

	for (qsizetype i = 0; i < body.length(); i++) {
		auto c = body[i];

		if (!quote.isNull()) {
			if (c == quote) quote = QChar();
			continue;
		}

		if (c == '"' || c == '\'') quote = c;
		else if (c == '{') depth++;
		else if (c == '}') depth--;
		else if (c == ',' && depth == 0) {
			fields.append(body.mid(start, i - start));
			start = i + 1;
		}
	}

	fields.append(body.mid(start));

	for (const auto& field: fields) {
		auto eq = field.indexOf('=');
		if (eq == -1) continue;
		call.table.insert(field.left(eq).trimmed(), Dispatcher::unquote(field.mid(eq + 1)));
	}

	return call;
}

void Dispatcher::dispatchLua(const LuaCall& call) {
	const auto& fn = call.function;
	const auto& t = call.table;
	auto windowArg = t.value("window");

	if (fn == "hl.dsp.focus") {
		if (t.contains("workspace")) this->focusWorkspace(t.value("workspace"));
		else if (t.contains("window")) {
			if (auto* window = this->resolveWindow(windowArg)) window->activate();
		} else if (t.contains("monitor")) this->unsupported("focus monitor");
		else this->unsupported(fn);
	} else if (fn == "hl.dsp.window.move") {
		auto* window = this->resolveWindow(windowArg);
		if (t.contains("workspace")) {
			this->moveToWorkspace(window, t.value("workspace"), t.value("follow") != "false");
		} else if (t.contains("x") || t.contains("y")) {
			this->moveWindowPixel(window, t.value("x"), t.value("y"));
		} else if (t.contains("direction")) {
			this->moveWindow(t.value("direction"));
		} else {
			this->unsupported(fn);
		}
	} else if (fn == "hl.dsp.window.close") {
		if (auto* window = this->resolveWindow(windowArg)) window->close();
	} else if (fn == "hl.dsp.window.kill_active" || fn == "hl.dsp.kill_active") {
		if (auto* window = this->ipc->tracker()->activeWindow()) window->close();
	} else if (fn == "hl.dsp.window.pin") {
		this->pin(this->resolveWindow(windowArg));
	} else if (fn == "hl.dsp.window.fullscreen") {
		auto mode = t.value("mode");
		this->fullscreen(this->resolveWindow(windowArg), mode == "maximize" || mode == "1" ? 1 : 0);
	} else if (fn == "hl.dsp.window.maximize") {
		this->fullscreen(this->resolveWindow(windowArg), 1);
	} else if (fn == "hl.dsp.global") {
		emit this->ipc->dispatchGlobal(call.scalar);
	} else if (fn == "hl.dsp.exec" || fn == "hl.dsp.exec_cmd") {
		this->exec(call.scalar);
	} else if (fn.startsWith("hl.config")) {
		// Compositor settings (cursor warps and the like) have nothing to apply to here.
		qCDebug(logDispatch) << "Ignoring config change" << fn;
	} else if (fn == "hl.dsp.workspace.toggle_special" || fn == "hl.dsp.window.toggle_float"
	           || fn == "hl.dsp.window.float" || fn == "hl.dsp.window.tile")
	{
		this->unsupported(fn);
	} else {
		this->unknown(fn);
	}
}

void Dispatcher::dispatchClassic(const QString& name, const QString& args) {
	auto* tracker = this->ipc->tracker();

	if (name == "workspace") {
		this->focusWorkspace(args);
	} else if (name == "movetoworkspace" || name == "movetoworkspacesilent") {
		auto comma = args.indexOf(',');
		auto workspace = comma == -1 ? args : args.left(comma);
		auto window = comma == -1 ? QString() : args.mid(comma + 1);
		this->moveToWorkspace(this->resolveWindow(window), workspace, name == "movetoworkspace");
	} else if (name == "killactive") {
		if (auto* window = tracker->activeWindow()) window->close();
	} else if (name == "closewindow") {
		if (auto* window = this->resolveWindow(args)) window->close();
	} else if (name == "focuswindow") {
		if (auto* window = this->resolveWindow(args)) window->activate();
	} else if (name == "fullscreen" || name == "fullscreenstate") {
		this->fullscreen(tracker->activeWindow(), args.trimmed() == "1" ? 1 : 0);
	} else if (name == "pin") {
		this->pin(this->resolveWindow(args));
	} else if (name == "movefocus") {
		this->moveFocus(args);
	} else if (name == "movewindow") {
		this->moveWindow(args);
	} else if (name == "movewindowpixel") {
		// exact X Y,window
		auto comma = args.indexOf(',');
		auto coords = (comma == -1 ? args : args.left(comma)).split(' ', Qt::SkipEmptyParts);
		auto window = comma == -1 ? QString() : args.mid(comma + 1);
		if (coords.length() >= 3 && coords[0] == "exact") {
			this->moveWindowPixel(this->resolveWindow(window), coords[1], coords[2]);
		} else {
			this->unsupported("movewindowpixel (relative)");
		}
	} else if (name == "exec") {
		this->exec(args);
	} else if (name == "global") {
		emit this->ipc->dispatchGlobal(args.trimmed());
	} else if (name == "togglefloating" || name == "togglespecialworkspace" || name == "layoutmsg"
	           || name == "pseudo" || name == "togglesplit" || name == "swapwindow"
	           || name == "focusmonitor" || name == "cyclenext" || name == "centerwindow"
	           || name == "movecurrentworkspacetomonitor" || name == "resizeactive"
	           || name == "swapnext" || name == "togglegroup" || name == "submap")
	{
		this->unsupported(name);
	} else {
		this->unknown(name);
	}
}

qsizetype Dispatcher::resolveWorkspace(const QString& arg, bool& create) const {
	auto a = arg.trimmed();
	create = false;

	auto* desktops = this->ipc->desktops();
	auto current = desktops->currentIndex();
	auto count = desktops->count();

	if (a.isEmpty() || a.startsWith("special") || a == "previous" || a == "previous_per_monitor") {
		return -1;
	}

	if (a.startsWith("name:")) {
		auto* workspace = this->ipc->workspaceByName(a.mid(5));
		return workspace == nullptr ? -1 : workspace->bindableId().value() - 1;
	}

	if (a == "empty") {
		// Every desktop, not just the listed workspaces: empty desktops are left out of those.
		for (qsizetype i = 0; i < count; i++) {
			auto* workspace = this->ipc->workspaceById(static_cast<qint32>(i + 1));
			if (workspace != nullptr && workspace->toplevels()->valueList().isEmpty()) return i;
		}

		create = true;
		return count;
	}

	// +N / -N and the e (existing), r (on monitor), m (monitor) relative forms all mean the
	// same thing with global desktops. Relative moves stop at the ends instead of creating.
	auto relative = a;
	if (relative.startsWith('e') || relative.startsWith('r') || relative.startsWith('m')) {
		relative = relative.mid(1);
	}

	if (relative.startsWith('+') || relative.startsWith('-')) {
		auto ok = false;
		auto delta = relative.toInt(&ok);
		if (!ok) return -1;
		return qBound(qsizetype(0), current + delta, qMax(count - 1, qsizetype(0)));
	}

	auto ok = false;
	auto number = a.toInt(&ok);
	if (!ok || number < 1) return -1;

	create = true;
	return number - 1;
}

TrackedWindow* Dispatcher::resolveWindow(const QString& arg) const {
	auto a = arg.trimmed();
	auto* tracker = this->ipc->tracker();

	if (a.isEmpty() || a == "activewindow") return tracker->activeWindow();

	if (a.startsWith("address:")) {
		auto hex = a.mid(8);
		if (hex.startsWith("0x")) hex = hex.mid(2);
		auto ok = false;
		auto address = hex.toULongLong(&ok, 16);
		if (!ok) return nullptr;
		auto* hwnd = reinterpret_cast<HWND>(static_cast<quintptr>(address)); // NOLINT
		return tracker->windowFor(hwnd);
	}

	if (a.startsWith("pid:")) {
		auto pid = a.mid(4).toUInt();
		for (auto* window: tracker->windows()) {
			if (window->pid() == pid) return window;
		}

		return nullptr;
	}

	auto colon = a.indexOf(':');
	if (colon != -1) {
		auto kind = a.left(colon);
		auto pattern = QRegularExpression(a.mid(colon + 1));
		auto byTitle = kind == "title" || kind == "initialtitle";
		auto byClass = kind == "class" || kind == "initialclass";

		if (byTitle || byClass) {
			for (auto* window: tracker->windows()) {
				auto value = byClass ? window->appId() : window->title();
				if (value == a.mid(colon + 1) || pattern.match(value).hasMatch()) return window;
			}

			return nullptr;
		}
	}

	qCWarning(logDispatch) << "Unsupported window selector" << a;
	return nullptr;
}

void Dispatcher::focusWorkspace(const QString& arg) {
	auto create = false;
	auto index = this->resolveWorkspace(arg, create);

	if (index < 0) {
		this->unsupported("workspace " + arg.trimmed());
		return;
	}

	auto* desktops = this->ipc->desktops();
	if (index >= desktops->count() && (!create || !desktops->ensureCount(index + 1))) return;

	desktops->switchTo(index);
}

void Dispatcher::moveToWorkspace(TrackedWindow* window, const QString& arg, bool follow) {
	if (window == nullptr) return;

	auto create = false;
	auto index = this->resolveWorkspace(arg, create);

	if (index < 0) {
		this->unsupported("movetoworkspace " + arg.trimmed());
		return;
	}

	auto* desktops = this->ipc->desktops();
	if (index >= desktops->count() && (!create || !desktops->ensureCount(index + 1))) return;

	if (!window->moveToDesktop(index)) return;
	if (follow) window->activate();
}

void Dispatcher::fullscreen(TrackedWindow* window, int mode) {
	if (window == nullptr) return;

	// Hyprland: 0 = fullscreen, 1 = maximize. Both toggle.
	if (mode == 1) window->setMaximized(!window->maximized());
	else window->setFullscreen(!window->fullscreen());
}

void Dispatcher::pin(TrackedWindow* window) {
	if (window == nullptr) return;

	auto* desktops = this->ipc->desktops();
	if (!desktops->accessorLoaded()) {
		this->unsupported("pin (needs VirtualDesktopAccessor.dll)");
		return;
	}

	desktops->pinWindow(window->hwnd(), !desktops->isWindowPinned(window->hwnd()));
}

void Dispatcher::moveFocus(const QString& direction) {
	auto* tracker = this->ipc->tracker();
	auto* desktops = this->ipc->desktops();
	auto* active = tracker->activeWindow();
	auto current = desktops->currentIndex();

	QList<TrackedWindow*> candidates;
	for (auto* window: tracker->windows()) {
		if (window == active || window->minimized()) continue;
		if (window->desktop() != -1 && window->desktop() != current) continue;
		candidates.append(window);
	}

	if (candidates.isEmpty()) return;

	if (active == nullptr) {
		candidates.first()->activate();
		return;
	}

	// Nearest window whose center lies in the requested direction, preferring ones in line.
	auto origin = active->rect().center();
	TrackedWindow* best = nullptr;
	auto bestScore = std::numeric_limits<qint64>::max();

	for (auto* window: candidates) {
		auto delta = window->rect().center() - origin;
		qint64 primary = 0;
		qint64 secondary = 0;

		if (direction.startsWith('l')) {
			if (delta.x() >= 0) continue;
			primary = -delta.x();
			secondary = qAbs(delta.y());
		} else if (direction.startsWith('r')) {
			if (delta.x() <= 0) continue;
			primary = delta.x();
			secondary = qAbs(delta.y());
		} else if (direction.startsWith('u')) {
			if (delta.y() >= 0) continue;
			primary = -delta.y();
			secondary = qAbs(delta.x());
		} else if (direction.startsWith('d')) {
			if (delta.y() <= 0) continue;
			primary = delta.y();
			secondary = qAbs(delta.x());
		} else {
			this->unsupported("movefocus " + direction);
			return;
		}

		auto score = primary + secondary * 2;
		if (score < bestScore) {
			bestScore = score;
			best = window;
		}
	}

	if (best != nullptr) best->activate();
}

void Dispatcher::moveWindow(const QString& direction) {
	if (direction.startsWith("mon:")) {
		this->unsupported("movewindow mon:");
		return;
	}

	auto key = arrowKey(direction.trimmed());
	if (key == 0) {
		this->unsupported("movewindow " + direction);
		return;
	}

	// Win+Arrow: snap left/right, maximize up, restore/minimize down. The closest thing to
	// moving a window within a layout.
	sendWinShortcut(key);
}

void Dispatcher::moveWindowPixel(TrackedWindow* window, const QString& x, const QString& y) {
	if (window == nullptr) return;

	auto* screen = window->screen();
	if (screen == nullptr) return;

	// Coordinates are relative to the window's screen, like Hyprland's monitor local layout.
	auto origin = screen->geometry().topLeft();
	auto logical = origin + QPoint(qRound(x.toDouble()), qRound(y.toDouble()));
	window->moveTo(logical);
}

void Dispatcher::exec(const QString& command) {
	auto line = command.trimmed();
	if (line.isEmpty()) return;

	if (line.contains("%focused_window%")) {
		auto* active = this->ipc->tracker()->activeWindow();
		line.replace("%focused_window%", QString::number(active == nullptr ? 0 : active->pid()));
	}

	auto commandLine = line.toStdWString();
	auto home = QDir::toNativeSeparators(QDir::homePath()).toStdWString();

	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};

	// CREATE_NO_WINDOW keeps `cmd /c <gui app>` from flashing a console; GUI apps are unaffected.
	auto ok = CreateProcessW(
	    nullptr,
	    commandLine.data(),
	    nullptr,
	    nullptr,
	    FALSE,
	    CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
	    nullptr,
	    home.c_str(),
	    &startup,
	    &process
	);

	if (ok) {
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
		return;
	}

	// Not an executable: let the shell resolve URLs, documents and app aliases.
	QString file;
	QString params;

	if (line.startsWith('"')) {
		auto end = line.indexOf('"', 1);
		file = line.mid(1, end - 1);
		if (end != -1) params = line.mid(end + 1).trimmed();
	} else {
		auto space = line.indexOf(' ');
		file = space == -1 ? line : line.left(space);
		if (space != -1) params = line.mid(space + 1).trimmed();
	}

	auto wfile = file.toStdWString();
	auto wparams = params.toStdWString();

	auto result = ShellExecuteW(
	    nullptr,
	    L"open",
	    wfile.c_str(),
	    params.isEmpty() ? nullptr : wparams.c_str(),
	    home.c_str(),
	    SW_SHOWNORMAL
	);

	if (reinterpret_cast<INT_PTR>(result) <= 32) {
		qCWarning(logDispatch) << "exec failed for" << line << "error"
		                       << reinterpret_cast<INT_PTR>(result);
	}
}

void Dispatcher::unknown(const QString& what) {
	if (this->warned.contains(what)) return;
	this->warned.insert(what);
	qCWarning(logDispatch) << "Unknown dispatcher" << what << "(further uses are not logged)";
}

void Dispatcher::unsupported(const QString& what) {
	if (this->warned.contains(what)) return;
	this->warned.insert(what);
	qCInfo(logDispatch) << "Dispatcher" << what
	                    << "has no Windows equivalent; ignored (further uses are not logged)";
}

} // namespace qs::hyprland::ipc
