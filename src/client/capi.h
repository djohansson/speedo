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
#include <cstdbool>
extern "C"
{
#else
#include <stdbool.h>
#endif

CLIENT_API void CreateClient(CreateWindowFunc createWindowFunc, const struct PathConfig* paths);
CLIENT_API void DestroyClient(void);
CLIENT_API bool OnEventClient();

#ifdef __cplusplus
}
#endif
