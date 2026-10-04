#pragma once

#if defined(__WINDOWS__) && defined(GFX_DYNAMIC_LINKING)
#	if defined(GFX_DLL_EXPORT)
#		define GFX_API __declspec(dllexport)
#	else
#		define GFX_API __declspec(dllimport)
#	endif
#else
#	define GFX_API
#endif

#include <rhi/capi.h>

#ifdef __cplusplus
extern "C"
{
#endif

// forward to the running gfx::WindowedApplication, if any
GFX_API void ResizeFramebuffer(WindowHandle window, int width, int height);
GFX_API struct WindowState* GetWindowState(WindowHandle window);

// the window the application draws its user interface in, and parents its dialogues to. only the first window set
// counts; until then, kInvalidWindowHandle.
GFX_API WindowHandle GetCurrentWindow(void);
GFX_API void SetCurrentWindow(WindowHandle window);

#ifdef __cplusplus
}
#endif
