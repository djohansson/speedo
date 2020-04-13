#pragma once

#include <array>
#include <map>

#include "glm.h"

struct MouseState
{
    float xpos = 0.0f;
    float ypos = 0.0f;
    uint32_t button = 0;
    uint32_t action = 0;
    uint32_t mods = 0;
    bool insideWindow = false;
};

struct KeyboardState
{
    uint32_t key = 0;
    uint32_t scancode = 0;
    uint32_t action = 0;
    uint32_t mods = 0;
};

struct WindowState
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fullscreenWidth = 0;
    uint32_t fullscreenHeight = 0;
    uint32_t fullscreenRefresh = 0;
    bool fullscreenEnabled = false;
};

struct InputState
{
    std::map<uint32_t, bool> keysPressed;
	std::array<bool, 3> mouseButtonsPressed;
	std::array<glm::vec2, 2> mousePosition;
};
