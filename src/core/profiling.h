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

#include <tracy/Tracy.hpp>
