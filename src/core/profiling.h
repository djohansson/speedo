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

#include <version> // defines __cpp_lib_debugging, if the standard library provides <debugging>

#if defined(__cpp_lib_debugging) && __cpp_lib_debugging >= 202311L
#include <debugging>
#else
#if defined(__WINDOWS__)
// declared directly rather than via <windows.h>, which this widely included header would otherwise pull in everywhere
// (its macros clash with ours, e.g. capi.h's UINT(name) rewriting the SDK's `typedef UINT (CALLBACK *YIELDPROC)`).
// matches the SDK declaration (WINBASEAPI BOOL WINAPI IsDebuggerPresent(VOID)), so including both is fine.
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent(void);
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
		return false;
	}
}
#endif
#endif

#include <tracy/Tracy.hpp>
