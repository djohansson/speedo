#pragma once

#include <gfx/shaders/capi.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace gfx
{

// the space a light can reach, as a sphere
struct LightSphere
{
	glm::vec3 center{};
	float radius = 0.0F;
};

// the cosine of a spot light's outer cone angle (where its attenuation reaches 0, see LightData::spotOffset)
[[nodiscard]] inline float SpotCosOuter(const LightData& light) noexcept
{
	return std::clamp(light.spotScale > 0.0F ? -light.spotOffset / light.spotScale : 0.0F, -1.0F, 1.0F);
}

// where a point or spot light can reach, given how far (reach: its range, or less): a point light's sphere of that
// radius, a spot light's cone capped by it, in its smallest bounding sphere (Wronski's: around the cap's rim for cones
// wider than 45 degrees, else through the apex and the rim). none for a directional light, which reaches everywhere
[[nodiscard]] inline std::optional<LightSphere> LightReach(const LightData& light, float reach) noexcept
{
	if (light.type == LIGHT_TYPE_DIRECTIONAL)
		return std::nullopt;

	glm::vec3 position(light.positionRange[0], light.positionRange[1], light.positionRange[2]);
	if (light.type != LIGHT_TYPE_SPOT)
		return LightSphere{position, reach};

	glm::vec3 direction = glm::normalize(glm::vec3(light.direction[0], light.direction[1], light.direction[2]));
	float cosOuter = SpotCosOuter(light);
	if (cosOuter <= 0.0F) // 90 degrees or wider: the cone holds a hemisphere
		return LightSphere{position, reach};
	constexpr float kCos45 = 0.70710678F;
	if (cosOuter < kCos45)
	{
		float sinOuter = std::sqrt(std::max(1.0F - (cosOuter * cosOuter), 0.0F));
		return LightSphere{position + (direction * (reach * cosOuter)), reach * sinOuter};
	}
	float radius = reach / (2.0F * cosOuter);
	return LightSphere{position + (direction * radius), radius};
}

} // namespace gfx
