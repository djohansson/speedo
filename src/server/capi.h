#pragma once

#include <core/capi.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

void CreateServer(const PathConfig* paths);
void DestroyServer(void);
bool TickServer();
const char* GetServerName(void);

#ifdef __cplusplus
}
#endif
