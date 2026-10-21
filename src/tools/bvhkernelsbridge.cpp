#include "bvhkernelsbridge.h"

#include <slang-cpp-prelude.h>

#include <bit>

// the kernels (bvhkernels.generated.cpp): each runs its groups from varyingInput's start to its end
extern "C"
{
void BvhTrace(ComputeVaryingInput* varyingInput, void* entryPointParams, void* globalParams);
void BvhTrace2(ComputeVaryingInput* varyingInput, void* entryPointParams, void* globalParams);
}

namespace bvhkernels::bridge
{

namespace detail
{

using Float4 = Vector<float, 4>;

// the kernel's global parameters: as bvhkernels.slang declares them, in its order
struct Globals
{
	StructuredBuffer<Float4> nodes;
	StructuredBuffer<Float4> triangles;
	StructuredBuffer<Float4> rays;
	RWStructuredBuffer<Float4> hits;
};

} // namespace detail

std::vector<Hit> Trace(
	std::span<const std::array<float, 4>> nodes,
	std::span<const std::array<float, 4>> triangles,
	std::span<const Ray> rays,
	bool binary,
	ForEach forEach)
{
	using namespace detail;

	constexpr uint32_t kGroupSize = 64;
	auto groupCount = static_cast<uint32_t>((rays.size() + kGroupSize - 1) / kGroupSize);
	// whole groups of rays, the padding's ending where they start (they meet nothing)
	std::vector<Float4> rayData(static_cast<size_t>(groupCount) * kGroupSize * 2, Float4{0.0F, 0.0F, 0.0F, 0.0F});
	for (size_t rayIt = 0; rayIt < rays.size(); rayIt++)
	{
		const auto& ray = rays[rayIt];
		rayData[rayIt * 2] = Float4{ray.origin[0], ray.origin[1], ray.origin[2], ray.end};
		rayData[(rayIt * 2) + 1] = Float4{ray.direction[0], ray.direction[1], ray.direction[2], ray.anyHit ? 1.0F : 0.0F};
	}
	for (size_t rayIt = rays.size(); rayIt < rayData.size() / 2; rayIt++)
		rayData[(rayIt * 2) + 1] = Float4{0.0F, 0.0F, 1.0F, 0.0F};
	std::vector<Float4> hitData(rayData.size());

	Globals globals{};
	globals.nodes.data = reinterpret_cast<Float4*>(const_cast<std::array<float, 4>*>(nodes.data()));
	globals.nodes.count = nodes.size();
	globals.triangles.data = reinterpret_cast<Float4*>(const_cast<std::array<float, 4>*>(triangles.data()));
	globals.triangles.count = triangles.size();
	globals.rays.data = rayData.data();
	globals.rays.count = rayData.size();
	globals.hits.data = hitData.data();
	globals.hits.count = hitData.size();
	forEach(
		groupCount,
		[&](uint32_t group)
		{
			ComputeVaryingInput input{};
			input.startGroupID = {group, 0, 0};
			input.endGroupID = {group + 1, 1, 1};
			auto groupGlobals = globals;
			(binary ? BvhTrace2 : BvhTrace)(&input, nullptr, &groupGlobals);
		});

	std::vector<Hit> hits(rays.size());
	for (size_t rayIt = 0; rayIt < rays.size(); rayIt++)
	{
		const auto& first = hitData[rayIt * 2];
		hits[rayIt] = Hit{
			.t = first.x,
			.barycentrics = {first.y, first.z},
			.primitive = std::bit_cast<uint32_t>(first.w),
			.frontFace = hitData[(rayIt * 2) + 1].x != 0.0F,
			.capped = hitData[(rayIt * 2) + 1].y != 0.0F};
	}
	return hits;
}

} // namespace bvhkernels::bridge
