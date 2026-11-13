#pragma once

#if defined(__WINDOWS__) && defined(RHI_DYNAMIC_LINKING)
#	if defined(RHI_DLL_EXPORT)
#		define RHI_API __declspec(dllexport)
#	else
#		define RHI_API __declspec(dllimport)
#	endif
#else
#	define RHI_API
#endif

// the windows rhi draws to (WindowHandle) are the platform's
#include <platform/capi.h>

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
extern "C"
{
#else
#include <stddef.h>
#include <stdint.h>
#endif

enum GraphicsApi : uint8_t
{
	kVk = 0,
	kMetal = 1, // macOS, beside Vulkan while the backend grows (see docs/backends-plan.md)
};

struct SourceLocationData
{
	const char* name;
	const char* function;
	const char* file;
	uint32_t line;
	uint32_t color;
};


#ifdef __cplusplus
}
#endif
