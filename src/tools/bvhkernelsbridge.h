#pragma once

#include <core/std_extra.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// bvh.slang's traversal runs on the cpu (src/tools/bvhkernels.slang, compiled to c++), behind an interface of plain types,
// as the environment kernels' (see environmentkernelsbridge.h): slang's prelude declares many names globally
namespace bvhkernels::bridge
{

struct Ray
{
	std::array<float, 3> origin{};
	float end = 0.0F;
	std::array<float, 3> direction{};
	bool anyHit = false;
};

// what a ray met (see BvhHit): the distance (its end where nothing), the barycentrics, the primitive (~0 where nothing),
// whether it met the triangle's front, and whether the traversal stopped at its step cap
struct Hit
{
	float t = 0.0F;
	std::array<float, 2> barycentrics{};
	uint32_t primitive = ~0U;
	bool frontFace = false;
	bool capped = false;
};

// calls body(index) for each index below count, on several threads (the other side's core::TaskExecutor::ForEach)
using ForEach = core::std_extra::function_ref<void(uint32_t count, core::std_extra::function_ref<void(uint32_t index)> body)>;

// traces rays through a CWBVH's nodes and triangles (see gfx::bvh::Data), or with binary a BVH in the moving BVH's layout
// (gfx::bvh::BuildBinary), as the gpu does
[[nodiscard]] std::vector<Hit> Trace(
	std::span<const std::array<float, 4>> nodes,
	std::span<const std::array<float, 4>> triangles,
	std::span<const Ray> rays,
	bool binary,
	ForEach forEach);

} // namespace bvhkernels::bridge
