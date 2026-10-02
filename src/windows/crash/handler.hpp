#pragma once

#include <qtclasshelpermacros.h>

#include "../../core/instanceinfo.hpp"

// Same namespace/interface as src/crash/handler.hpp (the POSIX implementation) so the shared
// call sites in src/launch/{launch,main}.cpp don't need to know which platform they're on beyond
// picking the right header to include. See handler.cpp for how this one actually works: it's a
// Win32-native minidump handler, not a port of the POSIX fork+execve design.
namespace qs::crash {

class CrashHandler {
public:
	static void init();
	static void setRelaunchInfo(const RelaunchInfo& info);
};

} // namespace qs::crash
