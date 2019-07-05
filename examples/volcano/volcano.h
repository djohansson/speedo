#pragma once

#include "state.h"

#ifdef __cplusplus
extern "C"
{
#endif

int vkapp_create(void* view, int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight, const char* resourcePath);
void vkapp_draw();
void vkapp_resizeWindow(const window_state* state);
void vkapp_resizeFramebuffer(int width, int height);
void vkapp_mouse(const mouse_state* state);
void vkapp_keyboard(const keyboard_state* state);
void vkapp_destroy(void);

#ifdef __cplusplus
}
#endif
