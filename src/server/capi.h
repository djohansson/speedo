#pragma once

#include <core/capi.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

void CreateServer(const struct PathConfig* paths);
void DestroyServer(void);
bool TickServer();

#ifdef __cplusplus
}
#endif
