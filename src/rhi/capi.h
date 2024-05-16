#pragma once

#if defined(__WINDOWS__) && defined(RHI_DYNAMIC_LINKING)
#	if defined(RHI_DLL_EXPORT) && (RHI_DLL_EXPORT==1)
#		define RHI_API __declspec(dllexport)
#	else
#		define RHI_API __declspec(dllimport)
#	endif
#else
#	define RHI_API
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

enum GraphicsApi
{
	kVk = 0,
};

typedef void* WindowHandle;
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

typedef WindowHandle (*CreateWindowFunc)(struct WindowState* window);
static const WindowHandle kInvalidWindowHandle = NULL;

RHI_API void ResizeFramebuffer(WindowHandle window, int width, int height);
RHI_API WindowHandle* GetWindows(size_t* count);
RHI_API void SetWindows(WindowHandle* windows, size_t count);
RHI_API WindowHandle GetCurrentWindow(void);
RHI_API void SetCurrentWindow(WindowHandle window);
RHI_API struct WindowState* GetWindowState(WindowHandle window);

#ifdef __cplusplus
}
#endif
