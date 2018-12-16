#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

int vkapp_create(void* view, int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight, const char* resourcePath, bool verbose);
void vkapp_draw();
void vkapp_resize(int framebufferWidth, int framebufferHeight);
void vkapp_destroy(void);

#ifdef __cplusplus
}
#endif
