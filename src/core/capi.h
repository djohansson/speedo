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

// the input's vocabulary, the window system's translated into it by the platform (see platform/glfw.cpp): what a
// key, a mouse button or either's action is, and which modifiers were held
enum Key
{
	KEY_UNKNOWN,
	KEY_SPACE,
	KEY_0,
	KEY_1,
	KEY_2,
	KEY_3,
	KEY_4,
	KEY_5,
	KEY_6,
	KEY_7,
	KEY_8,
	KEY_9,
	KEY_A,
	KEY_B,
	KEY_C,
	KEY_D,
	KEY_E,
	KEY_F,
	KEY_G,
	KEY_H,
	KEY_I,
	KEY_J,
	KEY_K,
	KEY_L,
	KEY_M,
	KEY_N,
	KEY_O,
	KEY_P,
	KEY_Q,
	KEY_R,
	KEY_S,
	KEY_T,
	KEY_U,
	KEY_V,
	KEY_W,
	KEY_X,
	KEY_Y,
	KEY_Z,
	KEY_ESCAPE,
	KEY_ENTER,
	KEY_TAB,
	KEY_BACKSPACE,
	KEY_INSERT,
	KEY_DELETE,
	KEY_RIGHT,
	KEY_LEFT,
	KEY_DOWN,
	KEY_UP,
	KEY_PAGE_UP,
	KEY_PAGE_DOWN,
	KEY_HOME,
	KEY_END,
	KEY_F1,
	KEY_F2,
	KEY_F3,
	KEY_F4,
	KEY_F5,
	KEY_F6,
	KEY_F7,
	KEY_F8,
	KEY_F9,
	KEY_F10,
	KEY_F11,
	KEY_F12,
	KEY_LEFT_SHIFT,
	KEY_LEFT_CONTROL,
	KEY_LEFT_ALT,
	KEY_LEFT_SUPER,
	KEY_RIGHT_SHIFT,
	KEY_RIGHT_CONTROL,
	KEY_RIGHT_ALT,
	KEY_RIGHT_SUPER,
	KEY_COUNT,
};
#define INPUT_ACTION_RELEASE 0u
#define INPUT_ACTION_PRESS 1u
#define INPUT_ACTION_REPEAT 2u
#define MOUSE_BUTTON_LEFT 0u
#define MOUSE_BUTTON_RIGHT 1u
#define MOUSE_BUTTON_MIDDLE 2u
#define MOUSE_BUTTON_OTHER 3u // the others, which nothing uses
#define INPUT_MOD_SHIFT (1u << 0u)
#define INPUT_MOD_CONTROL (1u << 1u)
#define INPUT_MOD_ALT (1u << 2u)
#define INPUT_MOD_SUPER (1u << 3u)

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
	uint32_t mods; // INPUT_MOD_*
	uint8_t button; // MOUSE_BUTTON_*
	uint8_t action; // INPUT_ACTION_*
	uint8_t insideWindow : 1;
};

struct KeyboardEvent
{
	uint32_t scancode; // the window system's, of the physical key
	uint32_t mods; // INPUT_MOD_*
	uint16_t key; // enum Key
	uint8_t action; // INPUT_ACTION_*
};

CORE_API void UpdateMouse(const struct MouseEvent* state);
CORE_API void UpdateKeyboard(const struct KeyboardEvent* state);
CORE_API const char* GetApplicationName(void);

#ifdef __cplusplus
}
#endif
