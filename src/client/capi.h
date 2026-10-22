#pragma once

#if defined(__WINDOWS__) && defined(CLIENT_DYNAMIC_LINKING)
#	if defined(CLIENT_DLL_EXPORT)
#		define CLIENT_API __declspec(dllexport)
#	else
#		define CLIENT_API __declspec(dllimport)
#	endif
#else
#	define CLIENT_API
#endif

#include <core/capi.h>
#include <platform/capi.h>
#include <rhi/capi.h>

#ifdef __cplusplus
extern "C"
{
#else
#include <stdbool.h>
#endif

// the client, with its window: the window system initialized first (PlatformInit)
CLIENT_API void ClientCreate(const struct PathConfig* paths);
CLIENT_API void ClientDestroy(void);
CLIENT_API bool ClientMain();

#ifdef __cplusplus
}
#endif
