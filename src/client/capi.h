#pragma once

#include <core/capi.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

void client_create(
	const WindowState* window,
	const char* rootPath,
	const char* resourcePath,
	const char* userProfilePath);
void client_destroy(void);
bool client_tick();
void client_resizeWindow(const WindowState* state);
void client_resizeFramebuffer(uint32_t width, uint32_t height);
void client_mouse(const MouseState* state);
void client_keyboard(const KeyboardState* state);
const char* client_getAppName(void);

#ifdef __cplusplus
}
#endif
