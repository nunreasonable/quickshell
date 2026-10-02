// Windows crash handling: SetUnhandledExceptionFilter (+ the UCRT invalid parameter handler,
// std::terminate and SIGABRT) write a minidump with MiniDumpWriteDump, then relaunch the shell
// once, mirroring src/crash/'s user-facing behaviour (dump + recent log next to it, don't
// relaunch if we crashed within 10s of starting) with a Win32-native implementation.
//
// Design note (BUILD-ONLY MODE, can't exercise this on the VM yet): the dump is written
// in-process, from the filter itself, using the standard self-dump idiom
// (MiniDumpWriteDump(GetCurrentProcess(), ...)). A true out-of-process watchdog - a second
// process that debugs this one, or that wakes on a shared event and calls MiniDumpWriteDump
// against our PID from outside - would be more robust for a corrupted stack/heap (its own stack
// is guaranteed healthy, unlike ours after e.g. a stack overflow), closer to what the Linux
// handler gets from forking a coredump child before it does anything risky. It needs a
// handle/shared-memory handoff across a process boundary (inheritable handles or a named
// mapping, a ready/done event, deciding what happens if the watchdog itself is missing) that
// would ship untested; the in-process filter is the standard, well-documented approach (it's the
// one in Microsoft's own minidump samples) and is simple enough to read and trust without a VM.
// SetThreadStackGuarantee gives the filter a little reserved stack to run in after a stack
// overflow, which is the main residual risk of this approach - see docs/PORTING.md.
#include "handler.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cwchar>
#include <exception>
#include <iterator>

#include <qt_windows.h>

#include <dbghelp.h>
#include <io.h>

#include <qdatetime.h>
#include <qdir.h>
#include <qfile.h>
#include <qlogging.h>
#include <qstring.h>
#include <qtenvironmentvariables.h>
#include <qtextstream.h>

#include "../../core/debuginfo.hpp"
#include "../../core/instanceinfo.hpp"
#include "../../core/logcat.hpp"
#include "../../core/paths.hpp"

namespace qs::crash {

namespace {

QS_LOGGING_CATEGORY(logCrashHandler, "quickshell.crashhandler", QtWarningMsg);

// Everything the filter needs, built once at startup (QString/QDir are fine in init(), which
// runs on a healthy stack long before any crash) and copied into fixed buffers so the filter
// itself never allocates. Not thread-safe to populate twice; init() only runs once.
constexpr int PATH_CHARS = 1024;
bool gReady = false; // guards against a crash between process start and init() finishing
wchar_t gExePath[PATH_CHARS] = L"";
wchar_t gConfigPath[PATH_CHARS] = L"";
wchar_t gCrashDir[PATH_CHARS] = L"";
wchar_t gDumpPath[PATH_CHARS] = L"";
wchar_t gLaunchTimeStr[32] = L"0";
qint64 gLaunchTimeMs = 0;
QtMessageHandler gPreviousMessageHandler = nullptr;

// Re-entrancy guard: if a second thread faults (or the same one re-faults) while we're already
// writing a dump and relaunching, don't let it race us into launching two copies of the shell.
// It just waits to be killed by the first handler's TerminateProcess instead.
std::atomic<bool> gHandling {false};

qint64 currentEpochMs() {
	FILETIME ft {};
	GetSystemTimeAsFileTime(&ft);
	// FILETIME is 100ns ticks since 1601-01-01; Unix epoch is 11644473600s later.
	auto ticks = (static_cast<quint64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
	return static_cast<qint64>(ticks / 10000) - 11644473600000LL;
}

// Runs after the dump is already safely on disk, so it's fine to use Qt/heap-using APIs here:
// if this part itself crashes, we still kept the important artifact. Skipped entirely for stack
// overflow (called with ep == nullptr guard from the caller) to keep that path minimal.
void writeSupportingFiles(const wchar_t* reason, DWORD exceptionCode) {
	auto dir = QDir(QString::fromWCharArray(gCrashDir));

	// Copy the live detailed log (logging.cpp keeps CrashInfo::INSTANCE.logFd pointed at the
	// current log file - the early temp file before initFs(), the real log.qslog after) next to
	// the dump, same filename as the Linux reporter uses.
	if (CrashInfo::INSTANCE.logFd != -1) {
		auto logHandle = reinterpret_cast<HANDLE>(_get_osfhandle(CrashInfo::INSTANCE.logFd)); // NOLINT
		if (logHandle != INVALID_HANDLE_VALUE) {
			auto destPath = dir.filePath("log.qslog.log");
			auto* dest = CreateFileW(
			    reinterpret_cast<LPCWSTR>(destPath.utf16()),
			    GENERIC_WRITE,
			    0,
			    nullptr,
			    CREATE_ALWAYS,
			    FILE_ATTRIBUTE_NORMAL,
			    nullptr
			);

			if (dest != INVALID_HANDLE_VALUE) {
				SetFilePointer(logHandle, 0, nullptr, FILE_BEGIN); // NOLINT
				auto buf = std::array<char, 64 * 1024>();
				DWORD read = 0;
				while (ReadFile(logHandle, buf.data(), buf.size(), &read, nullptr) && read > 0) {
					DWORD written = 0;
					WriteFile(dest, buf.data(), read, &written, nullptr);
				}
				CloseHandle(dest);
			}
		}
	}

	auto reportFile = QFile(dir.filePath("report.txt"));
	if (reportFile.open(QFile::WriteOnly)) {
		auto stream = QTextStream(&reportFile);
		stream << qs::debuginfo::combinedInfo();
		stream << "\n===== Instance Information =====\n";
		stream << "Reason: " << QString::fromWCharArray(reason) << " (exception code 0x" << Qt::hex
		       << exceptionCode << Qt::dec << ")\n";
		stream << "Config Path: " << QString::fromWCharArray(gConfigPath) << '\n';
		stream << "Dump: " << QString::fromWCharArray(gDumpPath) << '\n';
	}
}

void relaunch(const wchar_t* reason) {
	// Don't let a debug test crash (see maybeTriggerDebugCrash below) loop forever: strip it from
	// the environment block CreateProcess is about to inherit.
	SetEnvironmentVariableW(L"QS_DEBUG_CRASH_TEST", nullptr);

	// What the new instance logs about this crash (checkCrashRelaunch in launch/main.cpp).
	SetEnvironmentVariableW(L"__QUICKSHELL_CRASH_RELAUNCH", L"1");
	SetEnvironmentVariableW(L"__QUICKSHELL_CRASH_LAUNCH_TIME", gLaunchTimeStr);
	SetEnvironmentVariableW(L"__QUICKSHELL_CRASH_REASON", reason);
	SetEnvironmentVariableW(L"__QUICKSHELL_CRASH_DUMP_PATH", gDumpPath);

	STARTUPINFOW si {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi {};

	// <=2 MAX_PATH-ish strings plus quotes/flag/space fits comfortably in 4x the path buffer.
	auto cmdLine = std::array<wchar_t, PATH_CHARS * 4>();
	// The config path is a file (shell.qml or another entry), which is what -p takes.
	swprintf(cmdLine.data(), cmdLine.size(), L"\"%ls\" -p \"%ls\"", gExePath, gConfigPath); // NOLINT

	if (CreateProcessW(
	        gExePath,
	        cmdLine.data(),
	        nullptr,
	        nullptr,
	        FALSE,
	        0,
	        nullptr,
	        nullptr,
	        &si,
	        &pi
	    ))
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
}

// The single funnel every hook (SEH filter, invalid parameter, terminate, SIGABRT) goes through.
// `ep` is null for everything except the SEH filter; MiniDumpWriteDump accepts that (it just
// won't tag a specific faulting thread/exception record in the dump). Always ends by
// terminating the process (not marked [[noreturn]]: TerminateProcess is not provably one, since
// it can in theory fail).
void handleCrash(const wchar_t* reason, EXCEPTION_POINTERS* ep) {
	auto exceptionCode = ep != nullptr ? ep->ExceptionRecord->ExceptionCode : 0;
	auto isStackOverflow = exceptionCode == EXCEPTION_STACK_OVERFLOW;

	if (gHandling.exchange(true)) {
		// Already being handled elsewhere (or we re-entered after faulting again while handling
		// the first fault); don't race it. It will TerminateProcess us shortly.
		Sleep(30000); // NOLINT
		TerminateProcess(GetCurrentProcess(), 1);
	}

	if (gReady) {
		auto* dumpFile = CreateFileW(
		    gDumpPath,
		    GENERIC_WRITE,
		    0,
		    nullptr,
		    CREATE_ALWAYS,
		    FILE_ATTRIBUTE_NORMAL,
		    nullptr
		);

		if (dumpFile != INVALID_HANDLE_VALUE) {
			MINIDUMP_EXCEPTION_INFORMATION exInfo {};
			MINIDUMP_EXCEPTION_INFORMATION* exInfoPtr = nullptr;

			if (ep != nullptr) {
				exInfo.ThreadId = GetCurrentThreadId();
				exInfo.ExceptionPointers = ep;
				exInfo.ClientPointers = FALSE;
				exInfoPtr = &exInfo;
			}

			// Normal + thread info, no full memory: enough to see every thread's stack without a
			// multi-hundred-MB dump of the whole address space.
			MiniDumpWriteDump(
			    GetCurrentProcess(),
			    GetCurrentProcessId(),
			    dumpFile,
			    static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo),
			    exInfoPtr,
			    nullptr,
			    nullptr
			);

			CloseHandle(dumpFile);
		}

		// Relaunch before anything that needs the heap or locks the faulting thread may hold:
		// if writing the supporting files hangs, the shell is already back.
		auto elapsed = currentEpochMs() - gLaunchTimeMs;
		if (gLaunchTimeMs == 0 || elapsed >= 10000) {
			relaunch(reason);
		}
		// else: crashed within 10s of launch - matches the Linux handler's crash-loop guard,
		// don't relaunch.

		// Keep the stack-overflow path to just the dump above: dbghelp already needed the
		// SetThreadStackGuarantee reserve, no sense spending more of it on QFile/QDir.
		if (!isStackOverflow) {
			writeSupportingFiles(reason, static_cast<DWORD>(exceptionCode));
		}
	}

	TerminateProcess(GetCurrentProcess(), 1);
}

LONG WINAPI topLevelFilter(EXCEPTION_POINTERS* ep) {
	handleCrash(L"exception", ep);
	return EXCEPTION_EXECUTE_HANDLER; // unreached unless TerminateProcess itself somehow failed
}

void invalidParameterHandler(
    const wchar_t* /*expr*/,
    const wchar_t* /*func*/,
    const wchar_t* /*file*/,
    unsigned int /*line*/,
    uintptr_t /*reserved*/
) {
	handleCrash(L"invalid-parameter", nullptr);
}

void terminateHandler() { handleCrash(L"terminate", nullptr); }

void abortHandler(int /*sig*/) { handleCrash(L"abort", nullptr); }

// NOLINTNEXTLINE(misc-no-recursion) - deliberately recursing to test the stack overflow path.
// Returns something derived from the recursive call so it can't be rewritten as a tail call
// (and thus a plain loop that never actually grows the stack) by the optimizer. The infinite
// recursion is the point, not a bug - silence the (correct) warning locally.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion"
#endif
__declspec(noinline) int debugForceStackOverflow(int depth) {
	volatile char padding[4096]; // NOLINT
	padding[0] = static_cast<char>(depth);
	return debugForceStackOverflow(depth + 1) + padding[0];
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// QS_DEBUG_CRASH_TEST=access-violation|stack-overflow|abort|terminate|invalid-parameter: an
// undocumented hook for provoking each handler on purpose, meant for VM testing since this
// couldn't be exercised in BUILD-ONLY MODE. Fires once, at the end of setRelaunchInfo() -
// relaunch() above strips the variable so the restarted shell doesn't loop.
//
// This runs within moments of startup, which on its own would only ever exercise the "crashed
// within 10s of launch, don't relaunch" branch of handleCrash(). QS_DEBUG_CRASH_DELAY_MS (an
// integer) sleeps that many milliseconds first - set it above 10000 to exercise the relaunch
// path instead, or leave it unset (or below 10000) to test the crash-loop guard itself.
void maybeTriggerDebugCrash() {
	auto mode = qEnvironmentVariable("QS_DEBUG_CRASH_TEST");
	if (mode.isEmpty()) return;

	auto delayMs = qEnvironmentVariable("QS_DEBUG_CRASH_DELAY_MS").toInt();
	if (delayMs > 0) {
		qCWarning(logCrashHandler) << "QS_DEBUG_CRASH_TEST set, waiting" << delayMs
		                           << "ms before crashing via:" << mode;
		Sleep(static_cast<DWORD>(delayMs));
	}

	qCWarning(logCrashHandler) << "QS_DEBUG_CRASH_TEST set, deliberately crashing via:" << mode;

	if (mode == "access-violation") {
		auto* p = static_cast<volatile int*>(nullptr);
		*p = 1; // NOLINT
	} else if (mode == "stack-overflow") {
		volatile auto sink = debugForceStackOverflow(0);
		(void) sink;
	} else if (mode == "abort") {
		abort();
	} else if (mode == "terminate") {
		std::terminate();
	} else if (mode == "invalid-parameter") {
		// A null format string is rejected by the UCRT's parameter validation in release builds
		// too, not just debug ones. The null is deliberate, not a bug - silence the (correct)
		// warning locally rather than obscuring it behind a variable.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnonnull"
#endif
		printf(nullptr); // NOLINT
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
	} else {
		qCWarning(logCrashHandler) << "Unknown QS_DEBUG_CRASH_TEST mode, ignoring:" << mode;
	}
}

} // namespace

void CrashHandler::init() {
	qCDebug(logCrashHandler) << "Starting crash handler...";

	// No SetErrorMode(SEM_NOGPFAULTERRORBOX): the filter below already ends the process before
	// Windows Error Reporting would ask, and the error mode is inherited by every app the shell
	// starts, which would then crash without a word.

	// qFatal ends in qAbort, which fails fast on Windows and skips the exception filter
	// (D3D device loss and scene graph init failures go through it).
	gPreviousMessageHandler = qInstallMessageHandler(
	    [](QtMsgType type, const QMessageLogContext& context, const QString& message) {
		    if (gPreviousMessageHandler != nullptr) gPreviousMessageHandler(type, context, message);
		    if (type == QtFatalMsg) handleCrash(L"qFatal", nullptr);
	    }
	);

	// Reserve a little stack so the filter has somewhere to run after a stack overflow trips the
	// guard page. Only covers the thread it's called on (the Qt GUI thread, where init() runs);
	// worker threads (WinRT MTA threads etc.) aren't covered. Untested - see the file header.
	auto guarantee = ULONG {64 * 1024};
	SetThreadStackGuarantee(&guarantee);

	SetUnhandledExceptionFilter(&topLevelFilter);
	_set_invalid_parameter_handler(&invalidParameterHandler);
	std::set_terminate(&terminateHandler);
	signal(SIGABRT, &abortHandler); // NOLINT (misc-include-cleaner)

	qCInfo(logCrashHandler) << "Crash handler initialized.";
}

namespace {
// QString::toWCharArray() writes exactly size() wchar_t's with no bounds checking and no null
// terminator, so every copy into a fixed buffer must truncate to the buffer's capacity first.
void copyToBuffer(const QString& str, wchar_t* buf, int capacity) {
	auto truncated = str.left(capacity - 1);
	truncated.toWCharArray(buf);
	buf[truncated.size()] = L'\0';
}
} // namespace

void CrashHandler::setRelaunchInfo(const RelaunchInfo& info) {
	// Everything here gets copied into fixed-size buffers up front so the filter never has to
	// touch QString/QDir, which could themselves need the heap or crash again on a corrupted one.
	GetModuleFileNameW(nullptr, gExePath, PATH_CHARS);

	copyToBuffer(info.instance.configPath, gConfigPath, PATH_CHARS);

	gLaunchTimeMs = info.instance.launchTime.toMSecsSinceEpoch();
	swprintf(gLaunchTimeStr, std::size(gLaunchTimeStr), L"%lld", static_cast<long long>(gLaunchTimeMs)); // NOLINT

	auto dir = QsPaths::crashDir(info.instance.instanceId);
	if (dir.mkpath(".")) {
		copyToBuffer(dir.path(), gCrashDir, PATH_CHARS);

		auto dumpPath =
		    dir.filePath(info.instance.instanceId + "-" + QString::number(gLaunchTimeMs) + ".dmp");
		copyToBuffer(dumpPath, gDumpPath, PATH_CHARS);

		gReady = true;
		qCDebug(logCrashHandler) << "Crash dump path ready:" << dumpPath;
	} else {
		qCCritical(logCrashHandler
		) << "Failed to create crash directory, crash dumps will not be available.";
	}

	// Only once gReady (so a deliberate test crash goes through the same dump+relaunch path as a
	// real one) and everything above is populated.
	maybeTriggerDebugCrash();
}

} // namespace qs::crash
