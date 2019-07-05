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
    int fullscreen_width;
    int fullscreen_height;
    int fullscreen_refresh;
    bool fullscreen_enabled;
};

#ifdef __cplusplus
}
#endif
