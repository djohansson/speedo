#include "lightgizmos.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include <glm/geometric.hpp>
#include <glm/vector_relational.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace gfx
{

namespace lightgizmos
{

constexpr uint32_t kCircleSegments = 32;

// a circle about center, in the plane across axis
void AddCircle(LightGizmos& gizmos, glm::vec3 center, glm::vec3 axis, float radius, glm::vec4 color)
{
	glm::vec3 u = glm::normalize(glm::cross(axis, std::abs(axis.y) < 0.9F ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
	glm::vec3 v = glm::cross(axis, u);
	glm::vec3 previous = center + (u * radius);
	for (uint32_t segmentIt = 1; segmentIt <= kCircleSegments; segmentIt++)
	{
		float angle = 2.0F * std::numbers::pi_v<float> * static_cast<float>(segmentIt) / kCircleSegments;
		glm::vec3 point = center + (((u * std::cos(angle)) + (v * std::sin(angle))) * radius);
		gizmos.lines.push_back({previous, point, color});
		previous = point;
	}
}

// a shadow view's frustum: its clip space box (depth 0 to 1) back in world space
void AddFrustum(LightGizmos& gizmos, const ShadowData& view, glm::vec4 color)
{
	glm::mat4 inverse = glm::inverse(glm::make_mat4(&view.viewProjection[0][0]));
	std::array<glm::vec3, 8> corners;
	for (uint32_t cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		glm::vec4 clip(
			(cornerIt & 1U) != 0 ? 1.0F : -1.0F, (cornerIt & 2U) != 0 ? 1.0F : -1.0F, (cornerIt & 4U) != 0 ? 1.0F : 0.0F, 1.0F);
		glm::vec4 world = inverse * clip;
		corners[cornerIt] = glm::vec3(world) / world.w;
	}
	// the edges along x (corners differing in bit 0), y (bit 1) and depth (bit 2)
	for (uint32_t bit : {1U, 2U, 4U})
		for (uint32_t cornerIt = 0; cornerIt < 8; cornerIt++)
			if ((cornerIt & bit) == 0)
				gizmos.lines.push_back({corners[cornerIt], corners[cornerIt | bit], color});
}

} // namespace lightgizmos

uint32_t ShadowViewCount(uint32_t lightType)
{
	switch (lightType)
	{
	case LIGHT_TYPE_DIRECTIONAL: return SHADER_TYPES_SHADOW_CASCADE_COUNT;
	case LIGHT_TYPE_POINT: return 6;
	default: return 1;
	}
}

LightGizmos BuildLightGizmos(const LightCapture& capture, const LightGizmoOptions& options)
{
	using namespace lightgizmos;

	LightGizmos gizmos;
	glm::vec3 center(0.0F);
	float radius = 1.0F;
	glm::vec3 low(capture.sceneBounds.GetMin()[0], capture.sceneBounds.GetMin()[1], capture.sceneBounds.GetMin()[2]);
	glm::vec3 high(capture.sceneBounds.GetMax()[0], capture.sceneBounds.GetMax()[1], capture.sceneBounds.GetMax()[2]);
	if (glm::all(glm::lessThanEqual(low, high)))
	{
		center = (low + high) * 0.5F;
		radius = std::max(glm::length(high - low) * 0.5F, 1e-3F);
	}

	// the cascades and faces in colors of their own
	static constexpr std::array kViewColors{
		glm::vec4(1.0F, 0.35F, 0.35F, 0.9F),
		glm::vec4(0.35F, 1.0F, 0.35F, 0.9F),
		glm::vec4(0.4F, 0.6F, 1.0F, 0.9F),
		glm::vec4(1.0F, 1.0F, 0.35F, 0.9F),
		glm::vec4(1.0F, 0.4F, 1.0F, 0.9F),
		glm::vec4(0.35F, 1.0F, 1.0F, 0.9F),
	};

	for (size_t lightIt = 0; lightIt < capture.lights.size(); lightIt++)
	{
		if (options.light && *options.light != lightIt)
			continue;

		const auto& light = capture.lights[lightIt];
		glm::vec3 intensity(light.intensity[0], light.intensity[1], light.intensity[2]);
		float brightest = std::max({intensity.x, intensity.y, intensity.z, 1e-6F});
		glm::vec3 color = intensity / brightest;
		glm::vec4 lineColor(color, 1.0F);
		glm::vec3 direction(light.direction[0], light.direction[1], light.direction[2]);
		if (glm::length(direction) > 0.0F)
			direction = glm::normalize(direction);
		glm::vec3 position(light.positionRange[0], light.positionRange[1], light.positionRange[2]);
		float range = light.positionRange[3];

		switch (light.type)
		{
		case LIGHT_TYPE_DIRECTIONAL:
		{
			// no position: an arrow into the scene's center, along it (its marker is where it is in the sky, see Marker)
			glm::vec3 from = center - (direction * radius);
			gizmos.lines.push_back({from, center, lineColor});
			AddCircle(gizmos, center - (direction * (radius * 0.1F)), direction, radius * 0.03F, lineColor);
			position = -direction;
			break;
		}
		case LIGHT_TYPE_SPOT:
		{
			// its outer cone, where the attenuation reaches 0, out to its range (or the scene's size)
			if (light.spotScale > 0.0F)
			{
				float cosOuter = std::clamp(-light.spotOffset / light.spotScale, -1.0F, 1.0F);
				float length = range > 0.0F ? std::min(range, radius * 2.0F) : radius;
				float baseRadius = length * std::sqrt(std::max(1.0F - (cosOuter * cosOuter), 0.0F)) / std::max(cosOuter, 1e-3F);
				glm::vec3 base = position + (direction * length);
				AddCircle(gizmos, base, direction, baseRadius, lineColor);
				glm::vec3 u = glm::normalize(glm::cross(direction, std::abs(direction.y) < 0.9F ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
				glm::vec3 v = glm::cross(direction, u);
				for (glm::vec3 side : {u, -u, v, -v})
					gizmos.lines.push_back({position, base + (side * baseRadius), lineColor});
			}
			break;
		}
		case LIGHT_TYPE_POINT:
		{
			// its range, if it has one the scene's size can show
			if (range > 0.0F && range < radius * 4.0F)
				for (glm::vec3 axis : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)})
					AddCircle(gizmos, position, axis, range, glm::vec4(color, 0.4F));
			break;
		}
		default: break;
		}
		gizmos.markers.push_back({.position = position, .color = color, .type = light.type, .light = lightIt});

		if (!options.shadowFrusta || lightIt >= capture.lightShadows.size())
			continue;
		uint32_t first = capture.lightShadows[lightIt];
		if (first == SHADER_TYPES_NO_SHADOW)
			continue;
		for (uint32_t viewIt = 0; viewIt < ShadowViewCount(light.type) && first + viewIt < capture.shadowViews.size(); viewIt++)
			AddFrustum(gizmos, capture.shadowViews[first + viewIt], kViewColors[viewIt % kViewColors.size()]);
	}
	return gizmos;
}

} // namespace gfx
