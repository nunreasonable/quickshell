#include "input.hpp"

#include <qt_windows.h>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qstring.h>

#include "../../core/logcat.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logInput, "quickshell.windows.input", QtWarningMsg);

struct ScanCode {
	WORD scan;
	bool extended;
};

// evdev keycode (linux/input-event-codes.h) -> PC/AT Set 1 scancode. Most keys share the same
// byte value as their evdev keycode (that's how the evdev table was numbered in the first
// place); the "extended" keys below have an E0-prefixed scancode that collides with some other
// base key's byte, so they need an explicit entry.
bool scanCodeForEvdevKeycode(int keycode, ScanCode& out) {
	switch (keycode) {
	case 96: out = {0x1C, true}; return true; // KP Enter
	case 97: out = {0x1D, true}; return true; // Right Ctrl
	case 98: out = {0x35, true}; return true; // KP /
	case 99: out = {0x37, true}; return true; // Print Screen
	case 100: out = {0x38, true}; return true; // Right Alt
	case 102: out = {0x47, true}; return true; // Home
	case 103: out = {0x48, true}; return true; // Up
	case 104: out = {0x49, true}; return true; // Page Up
	case 105: out = {0x4B, true}; return true; // Left
	case 106: out = {0x4D, true}; return true; // Right
	case 107: out = {0x4F, true}; return true; // End
	case 108: out = {0x50, true}; return true; // Down
	case 109: out = {0x51, true}; return true; // Page Down
	case 110: out = {0x52, true}; return true; // Insert
	case 111: out = {0x53, true}; return true; // Delete
	case 125: out = {0x5B, true}; return true; // Left Meta/Win
	case 126: out = {0x5C, true}; return true; // Right Meta/Win
	case 127: out = {0x5D, true}; return true; // Menu/Compose
	default:
		// The rest of the common 104-key block (letters, digits, punctuation, function keys,
		// numpad digits, the left modifiers, tab/space/enter/backspace/esc, ...) uses the same
		// byte as its evdev keycode.
		if (keycode >= 1 && keycode <= 88) {
			out = {static_cast<WORD>(keycode), false};
			return true;
		}
		return false;
	}
}

void sendOne(WORD scan, bool extended, bool down) {
	INPUT input {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = 0;
	input.ki.wScan = scan;
	input.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0)
	                  | (down ? 0 : KEYEVENTF_KEYUP);

	if (SendInput(1, &input, sizeof(INPUT)) != 1) {
		qCWarning(logInput) << "SendInput failed:" << GetLastError();
	}
}

} // namespace

void Input::sendKey(int keycode, bool down) {
	ScanCode sc {};
	if (!scanCodeForEvdevKeycode(keycode, sc)) {
		qCWarning(logInput) << "sendKey: no scancode mapping for evdev keycode" << keycode;
		return;
	}
	sendOne(sc.scan, sc.extended, down);
}

void Input::sendText(const QString& text) {
	for (auto codeUnit: text) {
		INPUT down {};
		down.type = INPUT_KEYBOARD;
		down.ki.wVk = 0;
		down.ki.wScan = codeUnit.unicode();
		down.ki.dwFlags = KEYEVENTF_UNICODE;

		INPUT up = down;
		up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

		INPUT batch[2] = {down, up};
		SendInput(2, batch, sizeof(INPUT));
	}
}

} // namespace qs::windows::sys
