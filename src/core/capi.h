#pragma once

#if defined(__WINDOWS__) && defined(CORE_DYNAMIC_LINKING)
#	if defined(CORE_DLL_EXPORT) && (CORE_DLL_EXPORT==1)
#		define CORE_API __declspec(dllexport)
#	else
#		define CORE_API __declspec(dllimport)
#	endif
#else
#	define CORE_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

typedef struct _PathConfig
{
	const char* userProfilePath;
	const char* resourcePath;
} PathConfig;

typedef struct _MouseEvent
{
	enum
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
} MouseEvent;

typedef struct _KeyboardEvent
{
	uint32_t scancode;
	uint32_t mods;
	uint16_t key;
	uint8_t action;
} KeyboardEvent;

CORE_API void UpdateMouse(const MouseEvent* state);
CORE_API void UpdateKeyboard(const KeyboardEvent* state);
CORE_API const char* GetApplicationName(void);

#ifdef __cplusplus
}
#endif
