#include "keyboard_hook.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include <windows.h>

namespace qs::windows::hotkeys {

namespace {

// Shared between the gui thread and the hook thread.
std::atomic<std::shared_ptr<const HookSnapshot>> gSnapshot; // NOLINT
std::atomic<HWND> gTarget = nullptr;                         // NOLINT
std::atomic<UINT> gMessageBase = 0;                          // NOLINT
std::atomic<DWORD> gThreadId = 0;                            // NOLINT
std::atomic<bool> gInstalled = false;                        // NOLINT
// True while the foreground window belongs to a process we can't inject into (UIPI).
std::atomic<bool> gForegroundBlocked = false; // NOLINT

// Gui thread only. Heap allocated so static destruction never meets a joinable thread.
std::thread* gThread = nullptr; // NOLINT

// Hook thread only.
struct HeldKey {
	uint32_t id = 0;
	uint32_t serial = 0;
	DWORD lastTime = 0;
	bool active = false;
};

std::array<HeldKey, 256> gHeld {}; // NOLINT
uint8_t gTapCandidate = 0;         // NOLINT
DWORD gTapStart = 0;               // NOLINT
uint8_t gHoldActive = 0;           // NOLINT

// A swallowed key whose release we never saw (focus moved to an elevated window, the hook got
// dropped...) must not turn its next press into an auto-repeat. Real repeats arrive at most the
// maximum keyboard delay (1 s) apart.
constexpr DWORD STALE_REPEAT_MS = 1500;

bool isDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Inside the hook the async state does not include the event being processed yet.
uint8_t heldModifiers() {
	uint8_t mods = 0;
	if (isDown(VK_CONTROL)) mods |= ModCtrl;
	if (isDown(VK_MENU)) mods |= ModAlt;
	if (isDown(VK_SHIFT)) mods |= ModShift;
	if (isDown(VK_LWIN) || isDown(VK_RWIN)) mods |= ModSuper;
	return mods;
}

int otherSide(DWORD vk) {
	switch (vk) {
	case VK_LWIN: return VK_RWIN;
	case VK_RWIN: return VK_LWIN;
	case VK_LCONTROL: return VK_RCONTROL;
	case VK_RCONTROL: return VK_LCONTROL;
	case VK_LMENU: return VK_RMENU;
	case VK_RMENU: return VK_LMENU;
	case VK_LSHIFT: return VK_RSHIFT;
	case VK_RSHIFT: return VK_LSHIFT;
	default: return 0;
	}
}

bool isExtendedKey(uint8_t vk) {
	switch (vk) {
	case VK_INSERT:
	case VK_DELETE:
	case VK_HOME:
	case VK_END:
	case VK_PRIOR:
	case VK_NEXT:
	case VK_LEFT:
	case VK_RIGHT:
	case VK_UP:
	case VK_DOWN:
	case VK_LWIN:
	case VK_RWIN:
	case VK_APPS:
	case VK_RCONTROL:
	case VK_RMENU:
	case VK_DIVIDE:
	case VK_SNAPSHOT: return true;
	default: return vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2;
	}
}

INPUT keyInput(uint8_t vk, bool up, WORD scan = 0, bool extended = false) {
	INPUT input {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = vk;
	input.ki.wScan = scan;
	input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
	input.ki.dwExtraInfo = KeyboardHook::INJECTION_MARKER;
	return input;
}

// Any key event between a Win (Alt) press and its release keeps the Start menu (menu bar) from
// opening. Injected events queue behind the event being processed, so this is enough when the
// modifier is still held; a release has to be swallowed and re-sent behind the mask instead.
void injectMask() {
	std::array<INPUT, 2> inputs {
	    keyInput(KeyboardHook::MASK_KEY, false),
	    keyInput(KeyboardHook::MASK_KEY, true),
	};
	SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void injectMaskedRelease(const KBDLLHOOKSTRUCT* original) {
	std::array<INPUT, 3> inputs {
	    keyInput(KeyboardHook::MASK_KEY, false),
	    keyInput(KeyboardHook::MASK_KEY, true),
	    keyInput(
	        static_cast<uint8_t>(original->vkCode),
	        true,
	        static_cast<WORD>(original->scanCode),
	        (original->flags & LLKHF_EXTENDED) != 0
	    ),
	};
	SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void post(HookEvent event, uint32_t id, uint32_t serial) {
	auto* target = gTarget.load();
	if (target == nullptr) return;
	PostMessageW(target, gMessageBase.load() + event, id, static_cast<LPARAM>(serial));
}

void postAll(
    const HookSnapshot& snapshot,
    HookTrigger::Kind kind,
    uint8_t lone,
    HookEvent event
) {
	for (const auto& trigger: snapshot.triggers) {
		if (trigger.kind == kind && trigger.combo.lone == lone) post(event, trigger.id, snapshot.serial);
	}
}

bool hasTrigger(const HookSnapshot& snapshot, HookTrigger::Kind kind, uint8_t lone) {
	for (const auto& trigger: snapshot.triggers) {
		if (trigger.kind == kind && trigger.combo.lone == lone) return true;
	}
	return false;
}

DWORD integrityLevel(HANDLE token) {
	DWORD size = 0;
	GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
	if (size == 0) return 0;

	auto buffer = std::make_unique<uint8_t[]>(size);
	if (!GetTokenInformation(token, TokenIntegrityLevel, buffer.get(), size, &size)) return 0;

	auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.get());
	auto count = *GetSidSubAuthorityCount(label->Label.Sid);
	return count == 0 ? 0 : *GetSidSubAuthority(label->Label.Sid, count - 1);
}

DWORD ownIntegrityLevel() {
	static const DWORD level = []() {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return DWORD(0);
		auto level = integrityLevel(token);
		CloseHandle(token);
		return level;
	}();

	return level;
}

// UIPI hides input sent to windows of a higher integrity level from our hook and blocks our
// SendInput into them. A process we can't even query counts as hidden.
bool isHiddenFromUs(HWND hwnd) {
	if (hwnd == nullptr) return false;

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0 || pid == GetCurrentProcessId()) return false;

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process == nullptr) return true;

	auto hidden = true;
	HANDLE token = nullptr;
	if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
		auto level = integrityLevel(token);
		hidden = level == 0 || level > ownIntegrityLevel();
		CloseHandle(token);
	}

	CloseHandle(process);
	return hidden;
}

void CALLBACK foregroundChanged(
    HWINEVENTHOOK /*hook*/,
    DWORD /*event*/,
    HWND hwnd,
    LONG idObject,
    LONG /*idChild*/,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	if (idObject != OBJID_WINDOW) return;
	gForegroundBlocked.store(isHiddenFromUs(hwnd));
}

LRESULT onModifier(const HookSnapshot& snapshot, const KBDLLHOOKSTRUCT* info, uint8_t bit, bool up) {
	auto vk = static_cast<int>(info->vkCode);
	auto blocked = gForegroundBlocked.load();

	if (!up) {
		// auto-repeat of a held modifier
		if (isDown(vk)) return 0;

		if (gTapCandidate != 0) {
			// a second modifier: this is a combo, not a tap
			gTapCandidate = 0;
		} else if (!blocked && heldModifiers() == 0 && hasTrigger(snapshot, HookTrigger::Tap, bit)) {
			gTapCandidate = bit;
			gTapStart = info->time;
		}

		// Holds only observe, so they also run while the foreground is hidden from us. A press
		// is reported even if the last release was never seen (Win+L, elevated windows).
		if (!isDown(otherSide(info->vkCode))) {
			gHoldActive |= bit;
			postAll(snapshot, HookTrigger::Hold, bit, HookPressed);
		}

		return 0;
	}

	if ((gHoldActive & bit) != 0 && !isDown(otherSide(info->vkCode))) {
		gHoldActive &= ~bit;
		postAll(snapshot, HookTrigger::Hold, bit, HookReleased);
	}

	if (gTapCandidate != bit) return 0;
	gTapCandidate = 0;
	if (blocked) return 0;

	// Masked even after a long hold: with Super bound to the launcher, opening the Start menu
	// on release would be surprising. Ctrl+Esc still opens it.
	auto masked = bit == ModSuper || bit == ModAlt;
	if (masked) injectMaskedRelease(info);

	// After the injection: the re-sent release makes this process the source of the last input,
	// so the gui thread may take the foreground for the tap's action (verified on the target
	// with AllowSetForegroundWindow). Swallowed combos don't get that, even with a mask key
	// injected; panels use forceForegroundWindow there.
	if (info->time - gTapStart <= snapshot.tapTimeoutMs) {
		postAll(snapshot, HookTrigger::Tap, bit, HookTapped);
	}

	return masked ? 1 : 0;
}

LRESULT onKey(const HookSnapshot& snapshot, const KBDLLHOOKSTRUCT* info, bool up) {
	auto vk = static_cast<uint8_t>(info->vkCode);
	auto& held = gHeld.at(vk);

	if (up) {
		if (!held.active) return 0;
		held.active = false;
		post(HookReleased, held.id, held.serial);
		return 1;
	}

	gTapCandidate = 0;

	if (held.active) {
		if (info->time - held.lastTime <= STALE_REPEAT_MS) {
			held.lastTime = info->time;
			post(HookRepeated, held.id, held.serial);
			return 1;
		}

		held.active = false;
	}

	if (gForegroundBlocked.load()) return 0;

	auto mods = heldModifiers();

	// The secure attention sequence never reaches hooks, but don't even try.
	if (vk == VK_DELETE && (mods & ModCtrl) != 0 && (mods & ModAlt) != 0) return 0;

	for (const auto& trigger: snapshot.triggers) {
		if (trigger.kind != HookTrigger::Combo) continue;
		if (trigger.combo.vk != vk || trigger.combo.mods != mods) continue;

		held = HeldKey {.id = trigger.id, .serial = snapshot.serial, .lastTime = info->time, .active = true};
		post(HookPressed, trigger.id, snapshot.serial);

		// The swallowed key would otherwise leave a lone Win (Alt) press behind.
		if ((mods & (ModSuper | ModAlt)) != 0) injectMask();
		return 1;
	}

	return 0;
}

LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam) {
	if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wParam, lParam);

	auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam); // NOLINT(performance-no-int-to-ptr)

	// Our own mask keys and re-sent releases. Other injected input (on-screen keyboards,
	// automation tools) is handled like real input.
	if ((info->flags & LLKHF_INJECTED) != 0 && info->dwExtraInfo == KeyboardHook::INJECTION_MARKER) {
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	auto snapshot = gSnapshot.load(std::memory_order_acquire);
	if (snapshot == nullptr || info->vkCode > 0xFF) {
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	auto up = (info->flags & LLKHF_UP) != 0;
	auto bit = modifierBit(info->vkCode);

	auto swallow = bit != 0 ? onModifier(*snapshot, info, bit, up) : onKey(*snapshot, info, up);
	if (swallow != 0) return 1;

	return CallNextHookEx(nullptr, code, wParam, lParam);
}

void hookThreadMain(HANDLE readyEvent) {
	// Create the message queue before signaling so an early WM_QUIT from stop() isn't lost.
	MSG msg {};
	PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	gThreadId.store(GetCurrentThreadId());

	// Keystrokes wait for this thread; don't let busy normal priority threads delay them.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	auto* hook = SetWindowsHookExW(WH_KEYBOARD_LL, &hookProc, GetModuleHandleW(nullptr), 0);
	HWINEVENTHOOK foregroundHook = nullptr;

	if (hook != nullptr) {
		foregroundHook = SetWinEventHook(
		    EVENT_SYSTEM_FOREGROUND,
		    EVENT_SYSTEM_FOREGROUND,
		    nullptr,
		    &foregroundChanged,
		    0,
		    0,
		    WINEVENT_OUTOFCONTEXT
		);

		gForegroundBlocked.store(isHiddenFromUs(GetForegroundWindow()));
	}

	gInstalled.store(hook != nullptr);
	SetEvent(readyEvent);
	if (hook == nullptr) return;

	// Low level hooks are called from this thread's message loop.
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (foregroundHook != nullptr) UnhookWinEvent(foregroundHook);
	UnhookWindowsHookEx(hook);

	gHeld.fill({});
	gTapCandidate = 0;
	gHoldActive = 0;
}

} // namespace

uint8_t modifierBit(DWORD vk) {
	switch (vk) {
	case VK_LWIN:
	case VK_RWIN: return ModSuper;
	case VK_CONTROL:
	case VK_LCONTROL:
	case VK_RCONTROL: return ModCtrl;
	case VK_MENU:
	case VK_LMENU:
	case VK_RMENU: return ModAlt;
	case VK_SHIFT:
	case VK_LSHIFT:
	case VK_RSHIFT: return ModShift;
	default: return 0;
	}
}

bool KeyboardHook::start(HWND target, UINT messageBase) {
	gTarget.store(target);
	gMessageBase.store(messageBase);

	if (gThread != nullptr) return true;

	auto* ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (ready == nullptr) return false;

	gInstalled.store(false);
	gThread = new std::thread(&hookThreadMain, ready);
	WaitForSingleObject(ready, INFINITE);
	CloseHandle(ready);

	if (!gInstalled.load()) {
		gThread->join();
		delete gThread;
		gThread = nullptr;
		return false;
	}

	return true;
}

void KeyboardHook::stop() {
	if (gThread == nullptr) return;

	PostThreadMessageW(gThreadId.load(), WM_QUIT, 0, 0);
	gThread->join();
	delete gThread;
	gThread = nullptr;
	gTarget.store(nullptr);
}

bool KeyboardHook::isRunning() { return gThread != nullptr; }
bool KeyboardHook::foregroundBlocked() { return gForegroundBlocked.load(); }

void KeyboardHook::setSnapshot(std::shared_ptr<const HookSnapshot> snapshot) {
	gSnapshot.store(std::move(snapshot), std::memory_order_release);
}

bool KeyboardHook::sendKey(uint8_t vk) {
	auto scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
	auto extended = isExtendedKey(vk);

	std::array<INPUT, 2> inputs {
	    keyInput(vk, false, scan, extended),
	    keyInput(vk, true, scan, extended),
	};

	return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

} // namespace qs::windows::hotkeys
