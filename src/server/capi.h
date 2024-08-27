#pragma once

#include <core/capi.h>

#ifdef __cplusplus
#include <cstdbool>
extern "C"
{
#else
#include <stdbool.h>
#endif

void CreateServer(const struct PathConfig* paths);
void DestroyServer(void);
bool OnEventServer();

#ifdef __cplusplus
}
#endif
