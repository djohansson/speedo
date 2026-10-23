#pragma once

#include <gfx/bounds.h>
#include <gfx/shaders/capi.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// indirect lighting (see GiData in gfx/shaders/capi.h and gi.slang): world space irradiance probes over a voxel radiance
// volume. what the cpu plans: the volume's and the probes' layout over the scene, and the lights per cell of it
namespace gfx::gi
{

// how far a clip that follows the eye moves at a time: this many of its probes (see MakeClipmap; one of 8 voxels: the box
// around its center a clip holds is its half size less half a snap, and Sponza's thin clips lost all of their height to a
// snap of 4, so the finest clips lit nothing there)
inline constexpr uint32_t kSnapProbes = 1;

// the last clip's voxels along the scene's longest axis (see MakeClipmap): by default, and what View > Indirect Lighting
// Resolution offers. finer resolves light through narrower openings (the sky under Sponza's curtains, which 128 drew as
// spots at the corridor's end that faded as the eye neared them: the coarser clips' probes, 1.8 m apart; the surface
// cache resolves it at the voxels' size, see ComputeGiCacheFilter) at a steep cost: Sponza's corridor without the cache
// 2.1 ms a frame at 64, 2.6 at 96, 3.5 at 128, 5.4 at 192, 8.2 at 256, 11.9 at 320, 17.8 at 384 (RTX 4070 Ti SUPER,
// 1920x1080), the cache 0.7 ms more at 64, 1.4 at 96, 2.3 at 128, and little memory at 128, 0.3 GB at 384
// (Renderer::myGiCacheBuffer)
inline constexpr uint32_t kDefaultResolution = 128;
inline constexpr std::array<uint32_t, 7> kResolutions{64, 96, 128, 192, 256, 320, 384};
// what SPEEDO_GI_RESOLUTION may ask for
inline constexpr uint32_t kMinResolution = 8;
inline constexpr uint32_t kMaxResolution = 512;

// the near field (see GiData::nearContact): how near what a pixel's ray meets is a contact, in voxels of the clips that
// light the point, by default and at most (View > Indirect Lighting Near Field Contact, SPEEDO_GI_NEAR_CONTACT). 1.5 left
// the edges of the blocks under CornellBox's spheres faintly; wider takes more of the rays' light along every
// junction (Sponza's columns between the curtains lit a little more at 2.5)
inline constexpr float kDefaultNearContact = 2.5F;
inline constexpr float kMaxNearContact = 8.0F;

// a level's voxels per axis, of a volume of size voxels: rounded up, so that it covers all of it (as GiLevelVoxels)
[[nodiscard]] inline glm::uvec3 LevelVoxels(const glm::uvec3& size, uint32_t level)
{
	return glm::max((size + glm::uvec3((1U << level) - 1U)) >> level, glm::uvec3(1));
}

// a volume of size voxels from origin, and its probes: one every probeSpacing voxels. the counts are what the
// buffers hold for it: voxels, radiance texels (the frame's per voxel, then the direct radiance's), probes and
// flags (see GiData::flagFirst); its GiData's offsets are after first's
// counts (a clip before it in the buffers; the probes' halves and the rays' offsets are the renderer's, see
// GiData::probeFirst). origin is a whole number of voxels from anchor, the clipmap's corner
struct Layout
{
	GiData data{};
	uint32_t voxelCount = 0;
	uint32_t radianceCount = 0;
	uint32_t probeCount = 0;
	uint32_t flagCount = 0;
};
[[nodiscard]] Layout MakeLayout(
	const glm::vec3& origin, float voxelSize, const glm::uvec3& size, uint32_t probeSpacing, const Layout& first, const glm::vec3& anchor);

// the clipmap (see SHADER_TYPES_GI_CLIP_COUNT): clipCount volumes, finest first, of the voxel counts of the last, which is
// over bounds with at most resolution voxels along its longest axis (cubes) and a voxel of margin; each before it of
// voxels half the size of the next, centered ahead of eye along look (how far the camera looks, see
// FrameInputs::lookDistance), by lookAhead of its half size at most (snapped to its probes' grid, kept inside the last).
// total holds the counts of all (its data is unused)
struct Clipmap
{
	std::vector<Layout> clips;
	Layout total;
};
[[nodiscard]] Clipmap MakeClipmap(
	const Bounds3f& bounds, const glm::vec3& eye, const glm::vec3& look, float lookAhead, uint32_t resolution, uint32_t clipCount, uint32_t probeSpacing);

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
