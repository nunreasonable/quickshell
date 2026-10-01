# CLI11 is header only. Prefer an installed package. When cross compiling (or when no package
# is available) it can be pointed at a plain include directory, or, as a last resort, the bundled
# minimal replacement in src/launch/cli11 can be used with -DBUNDLED_CLI11=ON.
find_package(CLI11 CONFIG QUIET)

if (NOT TARGET CLI11::CLI11)
	set(CLI11_INCLUDE_DIR "" CACHE PATH "Directory containing CLI/CLI.hpp (header-only CLI11)")

	if (CLI11_INCLUDE_DIR)
		add_library(CLI11::CLI11 INTERFACE IMPORTED)
		target_include_directories(CLI11::CLI11 INTERFACE "${CLI11_INCLUDE_DIR}")
	elseif (BUNDLED_CLI11)
		message(WARNING "Using the bundled minimal CLI11 replacement (src/launch/cli11). "
			"Prefer the real CLI11 via find_package or CLI11_INCLUDE_DIR.")
		add_library(CLI11::CLI11 INTERFACE IMPORTED)
		target_include_directories(CLI11::CLI11 INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/cli11")
	else()
		find_package(CLI11 CONFIG REQUIRED)
	endif()
endif()

qt_add_library(quickshell-launch STATIC
	parsecommand.cpp
	command.cpp
	launch.cpp
	main.cpp
)

if (WIN32)
	target_sources(quickshell-launch PRIVATE daemon_win.cpp)
endif()

target_link_libraries(quickshell-launch PRIVATE
	Qt::Quick Qt::Widgets CLI11::CLI11 quickshell-build
)

qs_add_pchset(launch
	DEPENDENCIES Qt::Core CLI11::CLI11
	HEADERS
		<CLI/App.hpp>
		<qcoreapplication.h>
)

qs_pch(quickshell-launch SET launch)

target_link_libraries(quickshell PRIVATE quickshell-launch)
