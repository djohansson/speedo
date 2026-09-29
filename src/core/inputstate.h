#pragma once

#include <bitset>
#include <cstdint>

#include <glm/glm.hpp>

namespace core
{

struct InputState
{
	float dt = 0.0F;
	struct Keyboard
	{
		static constexpr size_t kKeyCount = 512;
		std::bitset<kKeyCount> keysDown;
	} keyboard{};
	struct Mouse
	{
		glm::vec2 position;
		glm::vec2 lastPosition;
		glm::vec2 leftLastPressPosition;
		glm::vec2 rightLastPressPosition;
		glm::vec2 middleLastPressPosition;
		uint8_t insideWindow : 1;
		uint8_t leftDown : 1;
		uint8_t rightDown : 1;
		uint8_t middleDown : 1;
	} mouse{};
};

} // namespace core
