#pragma once

#include <gfx/shaders/capi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace gfx
{
struct ModelDesc;
}

// emissive surfaces as lights (see EmitterTriangle, gfx/shaders/emitters.slang): a model's emissive triangles that don't
// move, which the main pass's pixels and the indirect lighting's voxels sample points of, through a tree of them, for
// the light they give, shadowed by rays through the BVHs
namespace gfx::emitters
{

// what a model's emitters are: its triangle lists' triangles whose material emits (its emissive factor, times its
// strength; or an animation sets it, see MaterialProperty::kEmissive), at each of their instances, in world space (their
// corners in the order whose cross product faces their front, a mirroring instance's swapped), and the tree the shaders
// pick one through (see EmitterNode), built by pbrt's surface area orientation heuristic over their bounds and power
// (area times the luminance of the emission factor alone: the texture's isn't known here, and the shaders weigh each
// point by its own). empty if none emits
struct Data
{
	std::vector<EmitterTriangle> triangles;
	std::vector<EmitterNode> nodes;
	double power = 0.0; // of all of them (luminance times area), for the log
	double area = 0.0;
};

[[nodiscard]] Data Build(const ModelDesc& desc, std::span<const uint32_t> indices, std::span<const VertexP3fN3fTa4fT014fC4f> vertices);

} // namespace gfx::emitters
