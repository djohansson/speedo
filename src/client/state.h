#pragma once

#include <core/glm.h>
#include <core/utils.h>

// this is old temp crap that should be reviewed/refactored/obliterated. in that order.
// original idea was that this would be pure c and part of the base application api.

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
	void* handle = nullptr;
	void* nativeHandle = nullptr;
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
	UnorderedMap<uint32_t, bool> keysPressed;
	bool mouseButtonsPressed[3];
	glm::vec2 mousePosition[2];
};
