#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct mouse_state_ mouse_state;
struct mouse_state_
{
    double xpos;
    double ypos;
    int button;
    int action;
    int mods;
    bool inside_window;
};

typedef struct keyboard_state_ keyboard_state;
struct keyboard_state_
{
    int key;
    int scancode;
    int action;
    int mods;
};

typedef struct window_state_ window_state;
struct window_state_
{
    int x;
    int y;
    int width;
    int height;
};

int vkapp_create(void* view, int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight, const char* resourcePath, bool verbose);
void vkapp_draw();
void vkapp_resizeWindow(const window_state* state);
void vkapp_resizeFramebuffer(int width, int height);
void vkapp_mouse(const mouse_state* state);
void vkapp_keyboard(const keyboard_state* state);
void vkapp_destroy(void);

#ifdef __cplusplus
}
#endif
