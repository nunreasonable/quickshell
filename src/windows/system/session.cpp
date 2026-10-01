#include "session.hpp"

#include <qt_windows.h>

#include <powrprof.h>
#include <winbase.h>
#include <winreg.h>

#include <qlogging.h>
#include <qloggingcategory.h>

#include "../../core/logcat.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logSession, "quickshell.windows.session", QtWarningMsg);

// Shutdown/restart (unlike logoff/lock/suspend) require the caller to hold SE_SHUTDOWN_NAME.
bool enablePrivilege(LPCWSTR name) {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
		return false;
	}

	LUID luid {};
	if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
		CloseHandle(token);
		return false;
	}

	TOKEN_PRIVILEGES tp {};
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	auto ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) != FALSE;
	CloseHandle(token);
	return ok && GetLastError() == ERROR_SUCCESS;
}

} // namespace

void Session::lock() {
	if (!LockWorkStation()) {
		qCWarning(logSession) << "LockWorkStation failed:" << GetLastError();
	}
}

void Session::logout() {
	if (!ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER)) {
		qCWarning(logSession) << "ExitWindowsEx(EWX_LOGOFF) failed:" << GetLastError();
	}
}

void Session::shutdown() {
	enablePrivilege(SE_SHUTDOWN_NAME);

	auto result = InitiateShutdownW(
	    nullptr,
	    nullptr,
	    0,
	    SHUTDOWN_POWEROFF,
	    SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER
	);

	if (result != ERROR_SUCCESS) {
		qCWarning(logSession) << "InitiateShutdownW(SHUTDOWN_POWEROFF) failed:" << result;
	}
}

void Session::reboot() {
	enablePrivilege(SE_SHUTDOWN_NAME);

	auto result = InitiateShutdownW(
	    nullptr,
	    nullptr,
	    0,
	    SHUTDOWN_RESTART,
	    SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER
	);

	if (result != ERROR_SUCCESS) {
		qCWarning(logSession) << "InitiateShutdownW(SHUTDOWN_RESTART) failed:" << result;
	}
}

void Session::suspend() {
	if (!SetSuspendState(FALSE, FALSE, FALSE)) {
		qCWarning(logSession) << "SetSuspendState(suspend) failed:" << GetLastError();
	}
}

void Session::hibernate() {
	if (!SetSuspendState(TRUE, FALSE, FALSE)) {
		qCWarning(logSession) << "SetSuspendState(hibernate) failed:" << GetLastError();
	}
}

void Session::rebootToFirmware() {
	// No documented user-mode API sets the "boot to firmware UI" EFI flag directly; shutdown.exe
	// does this through the same mechanism Settings > Advanced Startup uses internally.
	STARTUPINFOW si {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi {};

	wchar_t cmdLine[] = L"shutdown.exe /r /fw /t 0"; // NOLINT: CreateProcessW needs a mutable buffer

	if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		qCWarning(logSession) << "CreateProcessW(shutdown /r /fw) failed:" << GetLastError();
		return;
	}

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
}

bool Session::canHibernate() {
	SYSTEM_POWER_CAPABILITIES caps {};
	if (!GetPwrCapabilities(&caps)) return false;
	return caps.HiberFilePresent != FALSE;
}

} // namespace qs::windows::sys
