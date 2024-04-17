#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct _MouseState
{
	double xpos;
	double ypos;
	double xoffset;
	double yoffset;
	uint32_t mods;
	uint8_t button;
	uint8_t action;
	uint8_t insideWindow : 1;
} MouseState;

typedef struct _KeyboardState
{
	uint32_t scancode;
	uint32_t mods;
	uint16_t key;
	uint8_t action;
} KeyboardState;

typedef struct _WindowState
{
	void* handle;
	void* nativeHandle;
	float xscale; // content x scale factor
	float yscale; // content y scale factor
	uint32_t x; // screen x position. multiply by xscale to get framebuffer x position
	uint32_t y; // screen y position multiply by yscale to get framebuffer y position
	uint32_t width; // screen width. multiply by xscale to get framebuffer width
	uint32_t height; // screen height. multiply by yscale to get framebuffer height
	uint32_t fullscreenRefresh : 16;
	uint32_t fullscreenMonitor : 15;
	uint32_t fullscreenEnabled : 1;
} WindowState;

typedef struct _PathConfig
{
	const char* userProfilePath;
	const char* resourcePath;
} PathConfig;

#ifdef __cplusplus
}
#endif
