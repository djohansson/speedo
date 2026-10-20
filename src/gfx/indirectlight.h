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

// a volume of size voxels from origin, its levels, and its cascades: probes every probeSpacing voxels with
// octahedralSide^2 directions in cascade 0, as many cascades as reach as far as reach (world space). the counts are what
// the buffers hold for it: voxels (level 0), radiance texels (all levels, then the direct radiance's voxels), cascade
// texels (all cascades), cascade 0's probes and the probe masks; its GiData's offsets are after first's counts (a clip
// before it in the buffers)
struct Layout
{
	GiData data{};
	uint32_t voxelCount = 0;
	uint32_t radianceCount = 0;
	uint32_t cascadeTexelCount = 0;
	uint32_t probeCount = 0;
	uint32_t probeMaskCount = 0; // of all cascades but the last
};
[[nodiscard]] Layout MakeLayout(
	const glm::vec3& origin,
	float voxelSize,
	const glm::uvec3& size,
	uint32_t probeSpacing,
	uint32_t octahedralSide,
	float reach,
	const Layout& first);

// the clipmap (see SHADER_TYPES_GI_CLIP_COUNT): clipCount volumes, finest first, of the voxel counts of the last, which is
// over bounds with at most resolution voxels along its longest axis (cubes) and a voxel of margin; each before it of
// voxels half the size of the next, centered on eye (snapped to its probes' grid, kept inside the last), its cascades
// reaching across the last. total holds the counts of all (its data is unused)
struct Clipmap
{
	std::vector<Layout> clips;
	Layout total;
};
[[nodiscard]] Clipmap MakeClipmap(
	const Bounds3f& bounds,
	const glm::vec3& eye,
	uint32_t resolution,
	uint32_t clipCount,
	uint32_t probeSpacing,
	uint32_t octahedralSide);

// the lights (as the shader sees them) that may reach each cell of the volume's SHADER_TYPES_GI_LIGHT_GRID^3: per cell
// its first index and count (pairs of uints), and the indices. a directional light reaches every cell, a point or spot
// light those its reach's sphere (see LightReach) touches, one without a range as far as the volume's farthest corner
struct LightCells
{
	std::vector<uint32_t> ranges;
	std::vector<uint32_t> indices;
};
[[nodiscard]] LightCells BinLights(const GiData& data, std::span<const LightData> lights);
// each clip's cells after the one before's (GiData::lightCellFirst, which it sets), their indices after its indices
[[nodiscard]] LightCells BinLights(std::span<GiData> clips, std::span<const LightData> lights);

} // namespace gfx::gi
