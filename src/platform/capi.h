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
#include <stdbool.h>
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
	// how alt+enter goes fullscreen next (FULLSCREEN_MODE_*), chosen in the ui (View > Presentation) or with
	// SPEEDO_FULLSCREEN
	uint32_t fullscreenMode;
};

#define FULLSCREEN_MODE_BORDERLESS 0u // an undecorated window covering the monitor (on macOS, the menu bar and dock hidden)
#define FULLSCREEN_MODE_EXCLUSIVE 1u // glfwSetWindowMonitor: the default
#define FULLSCREEN_MODE_NATIVE 2u // macOS only: its own fullscreen space (toggleFullScreen:, the title bar's green button)

static const WindowHandle kInvalidWindowHandle = 0;//NOLINT(modernize-use-nullptr)

// the window system (glfw, see platform/glfw.cpp): initialized before the application is made (which makes its window,
// see platform::WindowedApplication) and terminated after it is gone. main thread
PLATFORM_API void PlatformInit(void);
PLATFORM_API void PlatformTerminate(void);
// waits for the window system's events, up to timeoutSeconds, and handles them: false once the window was asked to
// close. main thread
PLATFORM_API bool PlatformWaitEvents(double timeoutSeconds);
// wakes PlatformWaitEvents up. any thread (not a signal handler: not async signal safe)
PLATFORM_API void PlatformWakeMain(void);
// a vulkan surface of the window, for rhi (its types opaque here: platform knows nothing of graphics): instance a
// VkInstance, allocator a const VkAllocationCallbacks* (or null), surface a VkSurfaceKHR* it is written to. returns the
// VkResult
PLATFORM_API int32_t PlatformCreateVulkanSurface(WindowHandle window, void* instance, const void* allocator, void* surface);

// the window the application draws its user interface in, and parents its dialogues to. only the first window set
// counts; until then, kInvalidWindowHandle.
PLATFORM_API WindowHandle GetCurrentWindow(void);
PLATFORM_API void SetCurrentWindow(WindowHandle window);

#ifdef __cplusplus
}
#endif
