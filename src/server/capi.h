#pragma once

#if defined(__WINDOWS__) && defined(SERVER_DYNAMIC_LINKING)
#	if defined(SERVER_DLL_EXPORT)
#		define SERVER_API __declspec(dllexport)
#	else
#		define SERVER_API __declspec(dllimport)
#	endif
#else
#	define SERVER_API
#endif

#include <core/capi.h>

#ifdef __cplusplus
#include <cstdbool>
extern "C"
{
#else
#include <stdbool.h>
#endif

SERVER_API void ServerCreate(const struct PathConfig* paths);
SERVER_API void ServerDestroy(void);
SERVER_API bool ServerExitRequested();

#ifdef __cplusplus
}
#endif
