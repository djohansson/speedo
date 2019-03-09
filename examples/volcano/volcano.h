#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct mouse_state_ mouse_state;
struct mouse_state_
{
    double xpos, xpos_last;
    double ypos, ypos_last;
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

int vkapp_create(void* view, int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight, const char* resourcePath, bool verbose);
void vkapp_draw();
void vkapp_resize(int framebufferWidth, int framebufferHeight);
void vkapp_mouse(const mouse_state* state);
void vkapp_keyboard(const keyboard_state* state);
void vkapp_destroy(void);

#ifdef __cplusplus
}
#endif
