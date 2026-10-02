#include "system_stats.hpp"

#include <algorithm>

#include <pdhmsg.h>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qstring.h>
#include <qtimer.h>

#include "../../core/logcat.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logSystemStats, "quickshell.windows.systemstats", QtWarningMsg);

QString readCpuName() {
	wchar_t buffer[256] {};
	DWORD size = sizeof(buffer);

	auto status = RegGetValueW(
	    HKEY_LOCAL_MACHINE,
	    L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
	    L"ProcessorNameString",
	    RRF_RT_REG_SZ,
	    nullptr,
	    buffer,
	    &size
	);

	if (status != ERROR_SUCCESS) return QString();
	return QString::fromWCharArray(buffer).trimmed();
}

} // namespace

SystemStats::SystemStats(QObject* parent): QObject(parent) {
	this->mCpuName = readCpuName();
	this->bGpuUsage = -1.0;

	this->timer.setInterval(this->mUpdateIntervalMs);
	QObject::connect(&this->timer, &QTimer::timeout, this, &SystemStats::sample);

	if (this->mActive) {
		this->sample();
		this->timer.start();
	}
}

SystemStats::~SystemStats() { this->teardownGpuCounters(); }

void SystemStats::setActive(bool active) {
	if (this->mActive == active) return;
	this->mActive = active;
	emit this->activeChanged();

	if (active) {
		this->sample();
		if (this->mGpuEnabled) this->setupGpuCounters();
		this->timer.start();
	} else {
		this->timer.stop();
		this->teardownGpuCounters();
		this->havePrevCpuTimes = false;
	}
}

void SystemStats::setGpuEnabled(bool enabled) {
	if (this->mGpuEnabled == enabled) return;
	this->mGpuEnabled = enabled;
	emit this->gpuEnabledChanged();

	if (enabled && this->mActive) {
		this->setupGpuCounters();
	} else if (!enabled) {
		this->teardownGpuCounters();
		this->bGpuUsage = -1.0;
	}
}

void SystemStats::setUpdateIntervalMs(int interval) {
	if (interval <= 0 || this->mUpdateIntervalMs == interval) return;
	this->mUpdateIntervalMs = interval;
	this->timer.setInterval(interval);
	emit this->updateIntervalMsChanged();
}

void SystemStats::sample() {
	FILETIME idleTime {};
	FILETIME kernelTime {};
	FILETIME userTime {};

	if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
		ULARGE_INTEGER idle {};
		idle.LowPart = idleTime.dwLowDateTime;
		idle.HighPart = idleTime.dwHighDateTime;

		ULARGE_INTEGER kernel {};
		kernel.LowPart = kernelTime.dwLowDateTime;
		kernel.HighPart = kernelTime.dwHighDateTime;

		ULARGE_INTEGER user {};
		user.LowPart = userTime.dwLowDateTime;
		user.HighPart = userTime.dwHighDateTime;

		if (this->havePrevCpuTimes) {
			// lpKernelTime already includes idle time.
			auto idleDelta = idle.QuadPart - this->prevIdle.QuadPart;
			auto kernelDelta = kernel.QuadPart - this->prevKernel.QuadPart;
			auto userDelta = user.QuadPart - this->prevUser.QuadPart;
			auto totalDelta = kernelDelta + userDelta;

			if (totalDelta > 0) {
				auto busy = totalDelta - static_cast<long long>(idleDelta);
				this->bCpuUsage = std::clamp(qreal(busy) / qreal(totalDelta), 0.0, 1.0);
			}
		}

		this->prevIdle = idle;
		this->prevKernel = kernel;
		this->prevUser = user;
		this->havePrevCpuTimes = true;
	}

	MEMORYSTATUSEX mem {};
	mem.dwLength = sizeof(mem);
	if (GlobalMemoryStatusEx(&mem)) {
		this->bMemoryTotalKb = mem.ullTotalPhys / 1024;
		this->bMemoryAvailableKb = mem.ullAvailPhys / 1024;

		// ullTotalPageFile/ullAvailPageFile are commit limits that already include physical RAM;
		// subtracting approximates the page file (swap) contribution alone.
		auto swapTotal =
		    mem.ullTotalPageFile > mem.ullTotalPhys ? mem.ullTotalPageFile - mem.ullTotalPhys : 0;
		auto swapAvail =
		    mem.ullAvailPageFile > mem.ullAvailPhys ? mem.ullAvailPageFile - mem.ullAvailPhys : 0;

		this->bSwapTotalKb = swapTotal / 1024;
		this->bSwapAvailableKb = std::min(swapAvail / 1024, swapTotal / 1024);
	}

	this->bUptimeSeconds = static_cast<qint64>(GetTickCount64() / 1000);

	this->sampleGpu();
}

void SystemStats::setupGpuCounters() {
	if (this->gpuQuery != nullptr) return;

	if (PdhOpenQueryW(nullptr, 0, &this->gpuQuery) != ERROR_SUCCESS) {
		this->gpuQuery = nullptr;
		this->bGpuUsage = -1.0;
		return;
	}

	DWORD pathListSize = 0;
	const auto* wildcard = L"\\GPU Engine(*engtype_3D)\\Utilization Percentage";

	// First call with a null buffer reports the required size in characters.
	auto status = PdhExpandWildCardPathW(nullptr, wildcard, nullptr, &pathListSize, 0);

	if (status != PDH_MORE_DATA && status != ERROR_SUCCESS) {
		qCDebug(logSystemStats) << "No GPU engine counters available (PdhExpandWildCardPathW):"
		                        << Qt::hex << status;
		this->teardownGpuCounters();
		this->bGpuUsage = -1.0;
		return;
	}

	std::vector<wchar_t> pathList(pathListSize);
	status = PdhExpandWildCardPathW(nullptr, wildcard, pathList.data(), &pathListSize, 0);

	if (status != ERROR_SUCCESS) {
		this->teardownGpuCounters();
		this->bGpuUsage = -1.0;
		return;
	}

	for (const wchar_t* path = pathList.data(); *path != L'\0'; path += wcslen(path) + 1) {
		PDH_HCOUNTER counter = nullptr;
		if (PdhAddEnglishCounterW(this->gpuQuery, path, 0, &counter) == ERROR_SUCCESS) {
			this->gpuCounters.push_back(counter);
		}
	}

	this->gpuAvailable = !this->gpuCounters.empty();
	if (!this->gpuAvailable) {
		this->teardownGpuCounters();
		this->bGpuUsage = -1.0;
	}
}

void SystemStats::teardownGpuCounters() {
	if (this->gpuQuery != nullptr) {
		PdhCloseQuery(this->gpuQuery);
		this->gpuQuery = nullptr;
	}
	this->gpuCounters.clear();
	this->gpuAvailable = false;
}

void SystemStats::sampleGpu() {
	if (!this->gpuAvailable || this->gpuQuery == nullptr) return;

	if (PdhCollectQueryData(this->gpuQuery) != ERROR_SUCCESS) return;

	qreal sum = 0.0;
	for (auto* counter: this->gpuCounters) {
		PDH_FMT_COUNTERVALUE value {};
		if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS
		    && value.CStatus == ERROR_SUCCESS)
		{
			sum += value.doubleValue;
		}
	}

	this->bGpuUsage = std::clamp(sum / 100.0, 0.0, 1.0);
}

} // namespace qs::windows::sys
