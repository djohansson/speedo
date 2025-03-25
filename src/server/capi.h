#pragma once

#include <core/capi.h>

#ifdef __cplusplus
#include <cstdbool>
extern "C"
{
#else
#include <stdbool.h>
#endif

void ServerCreate(const struct PathConfig* paths);
void ServerDestroy(void);
bool ServerExitRequested();

#ifdef __cplusplus
}
#endif
