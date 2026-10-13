#pragma once

#include <gfx/bounds.h>
#include <gfx/shaders/capi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

// clustered lighting (see SHADER_TYPES_CLUSTER_COUNT in gfx/shaders/capi.h): which lights may reach each cluster of each
// view, planned on the cpu each frame from the views' view projections (what the gpu draws with) and the scene's bounds
namespace gfx::clusters
{

// a frame's clusters: per view its slicing (gClusterViews), per cluster its first index and count (gClusters, pairs of
// uints, SHADER_TYPES_CLUSTER_COUNT per view), and the light indices (gClusterLights)
struct ClusterPlan
{
	std::vector<ClusterView> views;
	std::vector<uint32_t> ranges;
	std::vector<uint32_t> lights;
};

// bins lights (as the shader sees them) into the clusters of each view. a directional light, or one without a range,
// reaches every cluster; a point light the clusters its range (a sphere: a box around it, conservatively) may touch,
// in the view's frustum, and a spot light those its cone may (the cone's bounding sphere, see LightReach). depth is
// sliced from the near plane to the scene's farthest point (or the far plane). views whose lights don't fit in
// SHADER_TYPES_CLUSTER_LIGHT_INDEX_COUNT get none (enabled 0: they loop over all lights)
[[nodiscard]] ClusterPlan Plan(std::span<const LightData> lights, std::span<const glm::mat4> viewProjections, const Bounds3f& sceneBounds);

} // namespace gfx::clusters
