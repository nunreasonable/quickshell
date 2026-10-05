#pragma once

#include <qt_windows.h>

#include <qtypes.h>

namespace qs::windows::recorder {

// QueryPerformanceCounter in 100 ns units: the clock Media Foundation timestamps use and the
// one WASAPI reports packet positions in (IAudioCaptureClient::GetBuffer's QPC position), so
// video ticks and audio packets line up without converting between clocks.
inline qint64 qpc100ns() {
	static const qint64 frequency = [] {
		LARGE_INTEGER value {};
		QueryPerformanceFrequency(&value);
		return static_cast<qint64>(value.QuadPart);
	}();

	LARGE_INTEGER counter {};
	QueryPerformanceCounter(&counter);
	auto ticks = static_cast<qint64>(counter.QuadPart);
	// Split to keep ticks * 10^7 from overflowing after a few days of uptime.
	return ticks / frequency * 10'000'000 + ticks % frequency * 10'000'000 / frequency;
}

} // namespace qs::windows::recorder
