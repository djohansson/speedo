#pragma once

#if (SPEEDO_PROFILING_LEVEL > 0)
#	ifndef TRACY_ENABLE
#		define TRACY_ENABLE
#	endif
#else
#	ifdef TRACY_ENABLE
#		undef TRACY_ENABLE
#	endif
#endif

#if defined(__cpp_lib_debugging) && __cpp_lib_debugging >= 202311L
#include <debugging>
#else
#if defined(__WINDOWS__)
#	include <windows.h>
namespace std
{
	inline bool is_debugger_present() noexcept //NOLINT(readability-identifier-naming)
	{
		return ::IsDebuggerPresent() != 0;
	}
}
#else
namespace std
{
	inline bool is_debugger_present() noexcept //NOLINT(readability-identifier-naming)
	{
		return ::isatty(STDERR_FILENO) && ::getenv("SPEEDO_DEBUG");
	}
}
#endif
#endif

#include <tracy/Tracy.hpp>
