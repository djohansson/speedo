#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct _MouseEvent
{
	enum
	{
		Position = 1 << 0,
		Button = 1 << 1,
		Scroll = 1 << 2,
		Window = 1 << 3,
	} flags;
	double xpos;
	double ypos;
	double xoffset;
	double yoffset;
	uint32_t mods;
	uint8_t button;
	uint8_t action;
	uint8_t insideWindow : 1;
} MouseEvent;

typedef struct _KeyboardEvent
{
	uint32_t scancode;
	uint32_t mods;
	uint16_t key;
	uint8_t action;
} KeyboardEvent;

typedef struct _WindowState
{
	void* handle;
	void* nativeHandle;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t fullscreenWidth;
	uint32_t fullscreenHeight;
	uint32_t fullscreenRefresh;
	uint8_t fullscreenEnabled : 1;
} WindowState;

typedef struct _PathConfig
{
	const char* userProfilePath;
	const char* resourcePath;
} PathConfig;

#ifdef __cplusplus
}
#endif
