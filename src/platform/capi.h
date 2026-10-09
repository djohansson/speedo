#pragma once

#if defined(__WINDOWS__) && defined(PLATFORM_DYNAMIC_LINKING)
#	if defined(PLATFORM_DLL_EXPORT)
#		define PLATFORM_API __declspec(dllexport)
#	else
#		define PLATFORM_API __declspec(dllimport)
#	endif
#else
#	define PLATFORM_API
#endif

#ifdef __cplusplus
#include <cstdint>
extern "C"
{
#else
#include <stdint.h>
#endif

typedef uintptr_t WindowHandle;//NOLINT(modernize-use-using)
struct WindowState
{
	float xscale;	 // content x scale factor
	float yscale;	 // content y scale factor
	uint32_t x;		 // screen x position. multiply by xscale to get framebuffer x position
	uint32_t y;		 // screen y position multiply by yscale to get framebuffer y position
	uint32_t width;	 // screen width. multiply by xscale to get framebuffer width
	uint32_t height; // screen height. multiply by yscale to get framebuffer height
	uint32_t fullscreenRefresh : 16;
	uint32_t fullscreenMonitor : 15;
	uint32_t fullscreenEnabled : 1;
};

typedef WindowHandle (*CreateWindowFunc)(struct WindowState* window);//NOLINT(modernize-use-using)
typedef void (*DestroyWindowFunc)(WindowHandle window);//NOLINT(modernize-use-using)
static const WindowHandle kInvalidWindowHandle = 0;//NOLINT(modernize-use-nullptr)

// forward to the running platform::WindowedApplication, if any
PLATFORM_API void ResizeFramebuffer(WindowHandle window, int width, int height);
PLATFORM_API struct WindowState* GetWindowState(WindowHandle window);

// the window the application draws its user interface in, and parents its dialogues to. only the first window set
// counts; until then, kInvalidWindowHandle.
PLATFORM_API WindowHandle GetCurrentWindow(void);
PLATFORM_API void SetCurrentWindow(WindowHandle window);

#ifdef __cplusplus
}
#endif
