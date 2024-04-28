#pragma once

#if defined(__WINDOWS__) && defined(CLIENT_DYNAMIC_LINKING)
#	if defined(CLIENT_DLL_EXPORT) && (CLIENT_DLL_EXPORT==1)
#		define CLIENT_API __declspec(dllexport)
#	else
#		define CLIENT_API __declspec(dllimport)
#	endif
#else
#	define CLIENT_API
#endif

#include <core/capi.h>
#include <rhi/capi.h>

#ifdef __cplusplus
extern "C"
{
#endif

CLIENT_API void client_create(CreateWindowFunc createWindowFunc, const PathConfig* paths);
CLIENT_API void client_destroy(void);
CLIENT_API bool client_tick();

#ifdef __cplusplus
}
#endif
