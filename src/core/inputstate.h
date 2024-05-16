#pragma once

#include <array>
#include <bitset>
#include <cstdint>

struct InputState
{
	struct Keyboard
	{
		static constexpr size_t kKeyCount = 512;
		std::bitset<kKeyCount> keysDown;
	} keyboard{};
	struct Mouse
	{
		static constexpr size_t kHistorySize = 2;
		std::array<float, kHistorySize> position;
		std::array<float, kHistorySize> leftLastEventPosition;
		std::array<float, kHistorySize> rightLastEventPosition;
		uint8_t insideWindow : 1;
		uint8_t leftDown : 1;
		uint8_t rightDown : 1;
	} mouse{};
};
