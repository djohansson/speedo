#include "clusters.h"

#include <gfx/frustum.h>
#include <gfx/lightbounds.h>

#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace gfx::clusters
{

namespace detail
{

constexpr uint32_t kTilesX = SHADER_TYPES_CLUSTER_TILES_X;
constexpr uint32_t kTilesY = SHADER_TYPES_CLUSTER_TILES_Y;
constexpr uint32_t kSlices = SHADER_TYPES_CLUSTER_SLICES;
// depth is sliced over this many units from the near plane (the first slices are thin, the last as deep as a 24th of
// log2(1 + kDepthUnits) of the range): a unit is the range over it
constexpr float kDepthUnits = 1023.0F;
// the boxes are widened by this much (of a tile, a slice), so that the shader's rounding near a boundary can't miss one
constexpr float kMargin = 1e-3F;

// a plane scaled to a unit normal (zero stays zero)
[[nodiscard]] glm::vec4 Normalized(const glm::vec4& plane)
{
	float length = glm::length(glm::vec3(plane));
	return length > 0.0F ? plane / length : glm::vec4(0.0F);
}

// the slice (fractional) of a distance from the near plane
[[nodiscard]] float Slice(const ClusterView& view, float distance)
{
	return std::log2(1.0F + (std::max(distance, 0.0F) * view.depthInverseUnit)) * view.depthScale;
}

} // namespace detail

ClusterPlan Plan(std::span<const LightData> lights, std::span<const glm::mat4> viewProjections, const Bounds3f& sceneBounds)
{
	using namespace detail;

	ZoneScopedN("gfx::clusters::Plan");

	auto viewCount = std::min<size_t>(viewProjections.size(), SHADER_TYPES_VIEW_COUNT);
	ClusterPlan plan;
	plan.views.resize(SHADER_TYPES_VIEW_COUNT);
	plan.ranges.assign(viewCount * SHADER_TYPES_CLUSTER_COUNT * 2, 0);

	std::array<glm::vec3, 8> sceneCorners;
	for (size_t i = 0; i < sceneCorners.size(); i++)
		sceneCorners[i] = glm::vec3(
			(i & 1U) != 0 ? sceneBounds.GetMax().x : sceneBounds.GetMin().x,
			(i & 2U) != 0 ? sceneBounds.GetMax().y : sceneBounds.GetMin().y,
			(i & 4U) != 0 ? sceneBounds.GetMax().z : sceneBounds.GetMin().z);

	// per view: each cluster's lights, as a count then as ranges into one list
	std::vector<std::array<uint32_t, 6>> boxes(lights.size()); // per light: tiles x, y and slices, first and last
	std::vector<bool> reaches(lights.size());
	for (size_t viewIt = 0; viewIt < viewCount; viewIt++)
	{
		const auto& viewProjection = viewProjections[viewIt];
		auto frustum = Frustum::FromViewProjection(viewProjection);
		auto& view = plan.views[viewIt];

		// depth: from the near plane along the view, to the scene's farthest point, or the far plane if it is nearer
		auto nearPlane = Normalized(frustum.planes[4]);
		auto farPlane = Normalized(frustum.planes[5]);
		if (nearPlane == glm::vec4(0.0F))
			continue;
		float range = 0.0F;
		for (const auto& corner : sceneCorners)
			range = std::max(range, glm::dot(glm::vec3(nearPlane), corner) + nearPlane.w);
		if (farPlane != glm::vec4(0.0F))
		{
			// the far plane's distance from the near plane's point nearest the origin
			glm::vec3 onNear = -nearPlane.w * glm::vec3(nearPlane);
			range = std::min(range, glm::dot(glm::vec3(farPlane), onNear) + farPlane.w);
		}
		range = std::max(range, 1e-4F);
		std::copy_n(&nearPlane[0], 4, view.depthPlane);
		view.depthInverseUnit = kDepthUnits / range;
		view.depthScale = static_cast<float>(kSlices) / std::log2(1.0F + kDepthUnits);

		// each light's box of clusters
		for (size_t lightIt = 0; lightIt < lights.size(); lightIt++)
		{
			const auto& light = lights[lightIt];
			auto& box = boxes[lightIt];
			box = {0, kTilesX - 1, 0, kTilesY - 1, 0, kSlices - 1};
			reaches[lightIt] = true;
			// what it can light: a point light's range, a spot light's cone in it (see LightReach)
			if (light.type == LIGHT_TYPE_DIRECTIONAL || light.positionRange[3] <= 0.0F)
				continue;
			auto reach = *LightReach(light, light.positionRange[3]);
			glm::vec3 center = reach.center;
			float radius = reach.radius;
			Bounds3f sphere;
			sphere.SetMin(center - glm::vec3(radius));
			sphere.SetMax(center + glm::vec3(radius));
			if (frustum.Excludes(sphere))
			{
				reaches[lightIt] = false;
				continue;
			}

			float distance = glm::dot(glm::vec3(nearPlane), center) + nearPlane.w;
			box[4] = static_cast<uint32_t>(std::clamp(std::floor(Slice(view, distance - radius) - kMargin), 0.0F, static_cast<float>(kSlices - 1)));
			box[5] = static_cast<uint32_t>(std::clamp(std::floor(Slice(view, distance + radius) + kMargin), 0.0F, static_cast<float>(kSlices - 1)));

			// the screen: the sphere's box's corners projected (all of it if one is behind the eye)
			glm::vec2 min(1e30F);
			glm::vec2 max(-1e30F);
			bool behind = false;
			for (const auto& corner : sphere.Corners())
			{
				auto clip = viewProjection * glm::vec4(corner, 1.0F);
				if (clip.w <= 1e-6F)
				{
					behind = true;
					break;
				}
				glm::vec2 ndc = glm::vec2(clip) / clip.w;
				min = glm::min(min, ndc);
				max = glm::max(max, ndc);
			}
			if (behind)
				continue;
			auto tile = [](float ndc, uint32_t tiles, float margin)
			{
				return static_cast<uint32_t>(
					std::clamp(std::floor((((ndc * 0.5F) + 0.5F) * static_cast<float>(tiles)) + margin), 0.0F, static_cast<float>(tiles - 1)));
			};
			box[0] = tile(min.x, kTilesX, -kMargin);
			box[1] = tile(max.x, kTilesX, kMargin);
			box[2] = tile(min.y, kTilesY, -kMargin);
			box[3] = tile(max.y, kTilesY, kMargin);
		}

		// the counts, then the ranges, then the lists (in light order within each cluster)
		auto* ranges = &plan.ranges[viewIt * SHADER_TYPES_CLUSTER_COUNT * 2];
		auto forEachCluster = [&](size_t lightIt, auto&& fn)
		{
			const auto& box = boxes[lightIt];
			for (uint32_t slice = box[4]; slice <= box[5]; slice++)
				for (uint32_t y = box[2]; y <= box[3]; y++)
					for (uint32_t x = box[0]; x <= box[1]; x++)
						fn((((slice * kTilesY) + y) * kTilesX) + x);
		};
		size_t total = 0;
		for (size_t lightIt = 0; lightIt < lights.size(); lightIt++)
			if (reaches[lightIt])
				forEachCluster(lightIt, [&](uint32_t cluster) { ranges[(cluster * 2) + 1]++; total++; });
		if (plan.lights.size() + total > SHADER_TYPES_CLUSTER_LIGHT_INDEX_COUNT)
		{
			std::fill_n(ranges, SHADER_TYPES_CLUSTER_COUNT * 2, 0U);
			continue;
		}
		auto first = static_cast<uint32_t>(plan.lights.size());
		for (uint32_t cluster = 0; cluster < SHADER_TYPES_CLUSTER_COUNT; cluster++)
		{
			ranges[cluster * 2] = first;
			first += ranges[(cluster * 2) + 1];
			ranges[(cluster * 2) + 1] = 0;
		}
		plan.lights.resize(first);
		for (size_t lightIt = 0; lightIt < lights.size(); lightIt++)
			if (reaches[lightIt])
				forEachCluster(
					lightIt,
					[&](uint32_t cluster)
					{ plan.lights[ranges[cluster * 2] + ranges[(cluster * 2) + 1]++] = static_cast<uint32_t>(lightIt); });
		view.enabled = 1;
	}

	return plan;
}

} // namespace gfx::clusters
