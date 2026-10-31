#pragma once

#include <gfx/bounds.h>
#include <gfx/shaders/capi.h>

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace gfx
{

// a frame's lights as the renderer used them, for debugging (see Renderer::CaptureLights): gLights (the model's lights,
// then the environment's dominant lights), the shadow views planned for them, and the scene's bounds they were fitted to
struct LightCapture
{
	std::vector<LightData> lights;
	size_t modelLightCount = 0; // the first ones are the model's, the rest the environment's
	std::vector<ShadowData> shadowViews;
	std::vector<uint32_t> lightShadows; // per light: its first shadow view, or SHADER_TYPES_NO_SHADOW
	uint32_t cascadeSets = 1; // a directional light's cascades, one set per view (all finest first: see shadows::Plan)
	Bounds3f sceneBounds;
};

// what to draw of a capture's lights: their markers, and the frusta of their shadow views. light: only that one's
struct LightGizmoOptions
{
	bool shadowFrusta = true;
	std::optional<size_t> light;
};

// the lights as world space lines (a directional light's arrow into the scene's center, a spot light's cone, a point
// light's range, the shadow views' frusta) and a marker per light
struct LightGizmos
{
	struct Line
	{
		glm::vec3 from;
		glm::vec3 to;
		glm::vec4 color;
	};
	// where an icon goes: a point or spot light's position, or for a directional light the direction towards it, a point
	// at infinity (where it is in the sky: project it with w = 0)
	struct Marker
	{
		glm::vec3 position;
		glm::vec3 color; // the light's, normalized to its brightest channel
		uint32_t type = LIGHT_TYPE_DIRECTIONAL;
		size_t light = 0;
	};
	std::vector<Line> lines;
	std::vector<Marker> markers;
};

// how many shadow views a light of a type has (see ShadowData): a directional light's cascades for each of cascadeSets views
[[nodiscard]] uint32_t ShadowViewCount(uint32_t lightType, uint32_t cascadeSets = 1);

[[nodiscard]] LightGizmos BuildLightGizmos(const LightCapture& capture, const LightGizmoOptions& options);

} // namespace gfx
