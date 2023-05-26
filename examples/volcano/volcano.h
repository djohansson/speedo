#pragma once

#include "state.h"

#ifdef __cplusplus
extern "C"
{
#endif

	int volcano_create(
		const WindowState* window,
		const char* rootPath,
		const char* resourcePath,
		const char* userProfilePath);
	bool volcano_tick();
	void volcano_resizeWindow(const WindowState* state);
	void volcano_resizeFramebuffer(int width, int height);
	void volcano_mouse(const MouseState* state);
	void volcano_keyboard(const KeyboardState* state);
	void volcano_destroy(void);
	const char* volcano_getAppName(void);
	const char* volcano_getRootPath(void);
	const char* volcano_getResourcePath(void);
	const char* volcano_getUserProfilePath(void);

#ifdef __cplusplus
}
#endif
