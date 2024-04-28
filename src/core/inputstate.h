#pragma once

#include <bitset>
#include <cstdint>

struct InputState
{
	struct Keyboard
	{
		std::bitset<512> keysDown;
	} keyboard{};
	struct Mouse
	{
		float position[2];
		uint8_t insideWindow : 1;
		uint8_t leftDown : 1;
		float leftLastEventPosition[2];
		uint8_t rightDown : 1;
		float rightLastEventPosition[2];
	} mouse{};
};
