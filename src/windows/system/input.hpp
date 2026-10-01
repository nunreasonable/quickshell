#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows::sys {

///! Synthesizes keyboard input (`SendInput`), for the on-screen keyboard (replaces `ydotool`).
/// Keys are addressed by Linux evdev keycode (`input-event-codes.h`), matching what the OSK QML
/// already carries around (it used to shell out to `ydotool key <keycode>:<0|1>`).
class Input: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Input(QObject* parent = nullptr): QObject(parent) {}

	/// Presses (or releases) the key with evdev keycode `keycode`. Unmapped keycodes are ignored.
	Q_INVOKABLE static void sendKey(int keycode, bool down);
	/// Types arbitrary Unicode text, independent of the active keyboard layout.
	Q_INVOKABLE static void sendText(const QString& text);
};

} // namespace qs::windows::sys
