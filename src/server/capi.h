#pragma once

#include <core/capi.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

void server_create(const PathConfig* paths);
void server_destroy(void);
bool server_tick();
const char* server_getAppName(void);

#ifdef __cplusplus
}
#endif
