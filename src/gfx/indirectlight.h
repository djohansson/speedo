#pragma once

#include <gfx/bounds.h>
#include <gfx/shaders/capi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

// indirect lighting (see GiData in gfx/shaders/capi.h and gi.slang): world space radiance cascades over a voxel radiance
// volume. what the cpu plans: the volume's and the cascades' layout over the scene, and the lights per cell of it
namespace gfx::gi
{

// a volume over bounds of at most resolution voxels along its longest axis (cubes), its levels, and its cascades: probes
// every probeSpacing voxels with octahedralSide^2 directions in cascade 0, as many cascades as reach across the volume.
// the counts are what the buffers hold: voxels (level 0), radiance texels (all levels), cascade texels (all cascades)
// and cascade 0's probes
struct Layout
{
	GiData data{};
	uint32_t voxelCount = 0;
	uint32_t radianceCount = 0;
	uint32_t cascadeTexelCount = 0;
	uint32_t probeCount = 0;
	uint32_t probeMaskCount = 0; // of all cascades but the last
};
[[nodiscard]] Layout MakeLayout(const Bounds3f& bounds, uint32_t resolution, uint32_t probeSpacing, uint32_t octahedralSide);

// the lights (as the shader sees them) that may reach each cell of the volume's SHADER_TYPES_GI_LIGHT_GRID^3: per cell
// its first index and count (pairs of uints), and the indices. a directional light reaches every cell, a point or spot
// light those its reach's sphere (see LightReach) touches, one without a range as far as the volume's farthest corner
struct LightCells
{
	std::vector<uint32_t> ranges;
	std::vector<uint32_t> indices;
};
[[nodiscard]] LightCells BinLights(const GiData& data, std::span<const LightData> lights);

} // namespace gfx::gi
