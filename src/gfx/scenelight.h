#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace gfx
{

// a light a model file defines (gltf KHR_lights_punctual), in world space (see LightData in gfx/shaders/capi.h)
struct SceneLight
{
	enum class Type : uint8_t
	{
		kDirectional,
		kPoint,
		kSpot,
	};

	std::string name;
	Type type = Type::kDirectional;
	std::array<float, 3> position{}; // point, spot
	std::array<float, 3> direction{0.0F, 0.0F, -1.0F}; // directional, spot: where it shines, unit length
	std::array<float, 3> color{1.0F, 1.0F, 1.0F}; // linear
	float intensity = 1.0F; // lux (directional) or candela (point, spot)
	float range = 0.0F; // point, spot: 0 for none
	float innerConeAngle = 0.0F; // spot, in radians
	float outerConeAngle = 0.7853982F;
};

} // namespace gfx
