#pragma once

#include <gfx/bounds.h>

#include <array>

#include <glm/glm.hpp>

namespace gfx
{

// a view's frustum as six planes in world space (xyz the inward normal, w the distance), from its view projection
// (Gribb and Hartmann): -w <= x, y <= w and 0 <= z <= w in Vulkan's clip space, the depth reversed: planes[4] (z >= 0) is
// the far plane, planes[5] (z <= w) the near. an infinite far plane comes out as (0, 0, 0, near), which culls nothing
struct Frustum
{
	std::array<glm::vec4, 6> planes{};

	[[nodiscard]] static Frustum FromViewProjection(const glm::mat4& viewProjection) noexcept
	{
		auto row = [&viewProjection](glm::length_t index)
		{ return glm::vec4(viewProjection[0][index], viewProjection[1][index], viewProjection[2][index], viewProjection[3][index]); };
		auto x = row(0);
		auto y = row(1);
		auto z = row(2);
		auto w = row(3);
		return Frustum{.planes = {w + x, w - x, w + y, w - y, z, w - z}};
	}

	// whether bounds are entirely outside a plane (so out of sight): conservative, a box near a corner of the frustum
	// may be outside it without being outside any one plane
	[[nodiscard]] bool Excludes(const Bounds3f& bounds) const noexcept
	{
		const auto& min = bounds.GetMin();
		const auto& max = bounds.GetMax();
		for (const auto& plane : planes)
		{
			// the corner furthest along the plane's normal
			glm::vec3 corner(plane.x >= 0.0F ? max.x : min.x, plane.y >= 0.0F ? max.y : min.y, plane.z >= 0.0F ? max.z : min.z);
			if (glm::dot(glm::vec3(plane), corner) + plane.w < 0.0F)
				return true;
		}
		return false;
	}
};

} // namespace gfx
