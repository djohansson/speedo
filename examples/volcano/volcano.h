#pragma once

#include "state.h"

#ifdef __cplusplus
extern "C"
{
#endif

int volcano_create(
    void* windowHandle,
    int windowWidth,
    int windowHeight,
    const char* rootPath,
    const char* resourcePath,
    const char* userProfilePath);
bool volcano_draw();
void volcano_resizeWindow(const WindowState* state);
void volcano_resizeFramebuffer(int width, int height);
void volcano_mouse(const MouseState* state);
void volcano_keyboard(const KeyboardState* state);
void volcano_destroy(void);
const char* volcano_getAppName(void);

#ifdef __cplusplus
}
#endif
