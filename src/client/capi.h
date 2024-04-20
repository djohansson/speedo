#pragma once

#include <core/capi.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

void client_create(CreateWindowFunc createWindowFunc, WindowState* window, const PathConfig* paths);
void client_destroy(void);
bool client_tick();
void client_resizeWindow(const WindowState* state);
void client_resizeFramebuffer(const WindowState* state);
void client_mouse(const MouseEvent* state);
void client_keyboard(const KeyboardEvent* state);
const char* client_getAppName(void);

#ifdef __cplusplus
}
#endif
