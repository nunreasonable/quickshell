#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <windows.h>

// Low level keyboard hook for global shortcuts that RegisterHotKey can't provide: combos with
// the Windows key (the shell owns most of them, and the hook runs before its hotkey processing)
// and lone modifier taps (Super alone opening the launcher instead of the Start menu).
//
// Plain Win32 and std only, so a console probe can link it without Qt.
//
// The hook runs on a dedicated thread with its own message loop. It never blocks: it matches
// key events against an immutable snapshot of the triggers and posts what happened to a window
// of the gui thread. Windows silently removes low level hooks that take too long to answer.
//
// Limitations:
// - UIPI: while an elevated (higher integrity) window is in the foreground, the hook of a
//   medium integrity process is not called and SendInput into that window is blocked. The hook
//   additionally tracks the foreground window and stays passive while it can't see it, so the
//   shell's own shortcuts keep working there. RegisterHotKey binds are not affected.
// - Ctrl+Alt+Del and Win+L are handled by the system before any hook and are never touched.
namespace qs::windows::hotkeys {

enum Modifier : uint8_t {
	ModCtrl = 1 << 0,
	ModAlt = 1 << 1,
	ModShift = 1 << 2,
	ModSuper = 1 << 3,
};

// A key with modifiers, or a modifier alone (vk == 0, `lone` holds that modifier's bit).
struct KeyCombo {
	uint8_t mods = 0;
	uint8_t vk = 0;
	uint8_t lone = 0;

	[[nodiscard]] bool operator==(const KeyCombo& other) const = default;
};

struct HookTrigger {
	enum Kind : uint8_t {
		// A key with exact modifiers: swallowed, reports press, auto-repeat and release.
		Combo,
		// A modifier pressed and released alone within the tap time: reports a tap. Super and
		// Alt releases are masked so the Start menu / menu bar doesn't open.
		Tap,
		// A modifier held down, whatever other modifiers are held. Never swallowed.
		Hold,
	};

	Kind kind = Combo;
	KeyCombo combo;
	uint32_t id = 0;
};

struct HookSnapshot {
	std::vector<HookTrigger> triggers;
	// Identifies the trigger set an event belongs to, so events queued before a reload are
	// dropped instead of firing whatever bind now has the same id.
	uint32_t serial = 0;
	uint32_t tapTimeoutMs = 1000;
};

// Messages posted to the target window: messageBase + HookEvent, wParam = trigger id,
// lParam = snapshot serial.
enum HookEvent : UINT {
	HookPressed = 0,
	HookRepeated = 1,
	HookReleased = 2,
	HookTapped = 3,
	HookEventCount = 4,
};

class KeyboardHook {
public:
	// Starts the hook thread (or retargets it). Returns false if the hook can't be installed.
	static bool start(HWND target, UINT messageBase);
	static void stop();
	[[nodiscard]] static bool isRunning();

	// Atomically replaces the triggers the hook matches.
	static void setSnapshot(std::shared_ptr<const HookSnapshot> snapshot);

	// Injects a press and release of `vk`, marked so the hook lets it through untouched.
	static bool sendKey(uint8_t vk);

	// dwExtraInfo of every event we inject.
	static constexpr ULONG_PTR INJECTION_MARKER = 0x5153484B; // "QSHK"
	// Unassigned virtual key used to break up a lone Win/Alt press.
	static constexpr uint8_t MASK_KEY = 0xE8;
};

[[nodiscard]] uint8_t modifierBit(DWORD vk);

} // namespace qs::windows::hotkeys
