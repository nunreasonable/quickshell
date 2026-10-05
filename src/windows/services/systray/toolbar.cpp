#include <cwchar>
#include <vector>

#include <qt_windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <qlogging.h>
#include <qloggingcategory.h>

#include "../../../core/logcat.hpp"
#include "../../util.hpp"
#include "hook.hpp"

// Explorer's notification area on Windows 10 (and in the first Windows 11 builds) is a set of
// toolbar controls whose buttons each carry, in their dwData, a pointer to explorer's own
// record of the icon. Its head is stable since Windows 7 and is what ManagedShell reads too:
// window, id, callback message, state and version. Windows 11's newer XAML tray has no such
// toolbars and this finds nothing there.
namespace qs::windows::services::systray {

namespace {

QS_LOGGING_CATEGORY(logTrayToolbar, "quickshell.windows.systray", QtWarningMsg);

// A hung explorer is skipped (SMTO_ABORTIFHUNG); a slow one isn't waited on for long.
constexpr UINT TOOLBAR_TIMEOUT_MS = 300;
// Way more than any notification area holds; bounds a toolbar answering nonsense.
constexpr LRESULT MAX_BUTTONS = 512;

// The head of explorer's per-icon record, in a 64 bit explorer.
struct TrayItemHead {
	HWND hwnd;
	UINT uid;
	UINT callbackMessage;
	DWORD state;
	UINT version;
};

bool hasClass(HWND hwnd, const wchar_t* name) {
	wchar_t cls[64] {};
	GetClassNameW(hwnd, cls, 64);
	return wcscmp(cls, name) == 0;
}

// The toolbars holding notification icons: under TrayNotifyWnd (its SysPager has the user's
// icons, a toolbar of its own the system ones) and in NotifyIconOverflowWindow (the hidden
// icons). Other toolbars on the taskbar (Links, Desktop...) carry other data and are left out.
std::vector<HWND> trayToolbars() {
	std::vector<HWND> toolbars;

	auto collect = [&](HWND root) {
		if (root == nullptr) return;

		EnumChildWindows(
		    root,
		    [](HWND child, LPARAM param) -> BOOL {
			    auto* list = reinterpret_cast<std::vector<HWND>*>(param); // NOLINT(performance-no-int-to-ptr)
			    if (!hasClass(child, TOOLBARCLASSNAMEW)) return TRUE;

			    auto* parent = GetParent(child);
			    if (hasClass(parent, L"SysPager") || hasClass(parent, L"TrayNotifyWnd")
			        || hasClass(parent, L"NotifyIconOverflowWindow"))
			    {
				    list->push_back(child);
			    }

			    return TRUE;
		    },
		    reinterpret_cast<LPARAM>(&toolbars)
		);
	};

	collect(explorerTaskbarWindow());

	HWND overflow = nullptr;
	while ((overflow = FindWindowExW(nullptr, overflow, L"NotifyIconOverflowWindow", nullptr)) != nullptr) {
		collect(overflow);
	}

	return toolbars;
}

} // namespace

std::vector<ExplorerIconData> readExplorerToolbars() {
	// The record layout above is the 64 bit one, and explorer matches the system.
	if constexpr (sizeof(void*) != 8) return {};

	auto toolbars = trayToolbars();
	if (toolbars.empty()) return {};

	DWORD pid = 0;
	GetWindowThreadProcessId(toolbars.front(), &pid);
	if (pid == 0) return {};

	// TB_GETBUTTON writes into the toolbar owner's memory, so the button goes through a buffer
	// in explorer. Same user and integrity level: no extra rights needed.
	auto* process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, pid);
	if (process == nullptr) {
		qCDebug(logTrayToolbar) << "Can't open explorer to read its tray toolbars:" << GetLastError();
		return {};
	}

	auto* remote = VirtualAllocEx(process, nullptr, sizeof(TBBUTTON), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (remote == nullptr) {
		CloseHandle(process);
		return {};
	}

	auto ask = [](HWND toolbar, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR* result) {
		return SendMessageTimeoutW(
		           toolbar,
		           msg,
		           wParam,
		           lParam,
		           SMTO_ABORTIFHUNG | SMTO_BLOCK,
		           TOOLBAR_TIMEOUT_MS,
		           result
		       )
		    != 0;
	};

	std::vector<ExplorerIconData> icons;

	for (auto* toolbar: toolbars) {
		DWORD owner = 0;
		GetWindowThreadProcessId(toolbar, &owner);
		if (owner != pid) continue;

		DWORD_PTR count = 0;
		if (!ask(toolbar, TB_BUTTONCOUNT, 0, 0, &count)) continue;

		for (DWORD_PTR i = 0; i < count && i < static_cast<DWORD_PTR>(MAX_BUTTONS); i++) {
			DWORD_PTR ok = FALSE;
			if (!ask(toolbar, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(remote), &ok) || ok == FALSE) {
				continue;
			}

			TBBUTTON button {};
			if (!ReadProcessMemory(process, remote, &button, sizeof(button), nullptr)) continue;
			if (button.dwData == 0) continue;

			TrayItemHead head {};
			if (!ReadProcessMemory(
			        process,
			        reinterpret_cast<const void*>(button.dwData), // NOLINT(performance-no-int-to-ptr)
			        &head,
			        sizeof(head),
			        nullptr
			    ))
			{
				continue;
			}

			// Anything else means this isn't the record we think it is.
			auto versionKnown = head.version == 0 || head.version == NOTIFYICON_VERSION
			                 || head.version == NOTIFYICON_VERSION_4;
			if (head.hwnd == nullptr || head.callbackMessage == 0 || !versionKnown) continue;
			if (IsWindow(head.hwnd) == 0) continue;

			icons.push_back({
			    .hwnd = head.hwnd,
			    .uid = head.uid,
			    .callbackMessage = head.callbackMessage,
			    .version = head.version,
			});
		}
	}

	VirtualFreeEx(process, remote, 0, MEM_RELEASE);
	CloseHandle(process);

	qCDebug(logTrayToolbar) << "Read" << icons.size() << "icons from" << toolbars.size()
	                        << "explorer tray toolbars";

	return icons;
}

} // namespace qs::windows::services::systray
