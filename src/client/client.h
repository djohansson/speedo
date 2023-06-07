#pragma once

#include "state.h"

#ifdef __cplusplus
extern "C"
{
#endif

	int client_create(
		const WindowState* window,
		const char* rootPath,
		const char* resourcePath,
		const char* userProfilePath);
	bool client_tick();
	void client_resizeWindow(const WindowState* state);
	void client_resizeFramebuffer(int width, int height);
	void client_mouse(const MouseState* state);
	void client_keyboard(const KeyboardState* state);
	void client_destroy(void);
	const char* client_getAppName(void);
	const char* client_getRootPath(void);
	const char* client_getResourcePath(void);
	const char* client_getUserProfilePath(void);

#ifdef __cplusplus
}
#endif
