#pragma once

#include <core/capi.h>

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
void client_resizeFramebuffer(int width, int height);
void client_mouse(const MouseState* state);
void client_keyboard(const KeyboardState* state);
const char* client_getAppName(void);

#ifdef __cplusplus
}
#endif
