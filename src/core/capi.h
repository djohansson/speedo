#pragma once

#if defined(__WINDOWS__) && defined(CORE_DYNAMIC_LINKING)
#	if defined(CORE_DLL_EXPORT)
#		define CORE_API __declspec(dllexport)
#	else
#		define CORE_API __declspec(dllimport)
#	endif
#else
#	define CORE_API
#endif

#ifdef __cplusplus
#include <cstdint>
extern "C"
{
#else
#include <stdint.h>
#endif

struct PathConfig
{
	const char* userProfilePath;
	const char* resourcePath;
};

struct MouseEvent
{
	enum : uint8_t
	{
		kPosition = 1 << 0,
		kButton = 1 << 1,
		kScroll = 1 << 2,
		kWindow = 1 << 3,
	} flags;
	double xpos;
	double ypos;
	double xoffset;
	double yoffset;
	uint32_t mods;
	uint8_t button;
	uint8_t action;
	uint8_t insideWindow : 1;
};

struct KeyboardEvent
{
	uint32_t scancode;
	uint32_t mods;
	uint16_t key;
	uint8_t action;
};

CORE_API void UpdateMouse(const struct MouseEvent* state);
CORE_API void UpdateKeyboard(const struct KeyboardEvent* state);
CORE_API const char* GetApplicationName(void);

#ifdef __cplusplus
}
#endif
