#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#define VKAPP_MOUSE_ARCBALL_ENABLE_FLAG (1 << 0)

int vkapp_create(void* view, int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight, const char* resourcePath, bool verbose);
void vkapp_draw();
void vkapp_resize(int framebufferWidth, int framebufferHeight);
void vkapp_mouse(double x, double y, int state);
void vkapp_destroy(void);

#ifdef __cplusplus
}
#endif
