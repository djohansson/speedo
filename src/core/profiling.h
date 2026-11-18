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
// (its macros clash with ours, e.g. min and max).
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

#if (SPEEDO_PROFILING_LEVEL > 0)
#include <cstdlib>
#include <string_view>

namespace core::profiling
{

// whether the zones and frames are recorded (SPEEDO_TRACY=0: not; the scripts' test runs set it). Tracy, built without
// its on-demand mode, keeps every event from the start until a profiler connects: a client left running grew by about
// 65 MB a second (a release build, no Tracy, stays flat), so tests don't record, and an interactive run records from its
// first frame, as before (the user's call, 2026-11-18)
[[nodiscard]] inline bool Enabled() noexcept
{
	static const bool kEnabled = []
	{
		const char* tracy = std::getenv("SPEEDO_TRACY");
		return tracy == nullptr || std::string_view(tracy) != "0";
	}();
	return kEnabled;
}

} // namespace core::profiling

// Tracy's zone and frame macros, active as recorded (the ones the code uses)
#undef ZoneScoped
#undef ZoneScopedN
#undef FrameMark
#define ZoneScoped SuppressVarShadowWarning(ZoneNamed(___tracy_scoped_zone, ::core::profiling::Enabled()))
#define ZoneScopedN(name) SuppressVarShadowWarning(ZoneNamedN(___tracy_scoped_zone, name, ::core::profiling::Enabled()))
#define FrameMark \
	do \
	{ \
		if (::core::profiling::Enabled()) \
			::tracy::Profiler::SendFrameMark(nullptr); \
	} while (false)
#endif
