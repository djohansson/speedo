#include "indirectlight.h"

#include <gfx/lightbounds.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <bit>
#include <cmath>

namespace gfx::gi
{

Layout MakeLayout(
	const glm::vec3& origin, float voxelSize, const glm::uvec3& size, uint32_t probeSpacing, const Layout& first, const glm::vec3& anchor)
{
	ENSURE(probeSpacing > 0 && voxelSize > 0.0F);
	// where the volume is from the anchor, in its voxels (whole ones, see MakeClipmap)
	glm::ivec3 originVoxels = glm::ivec3(glm::round((origin - anchor) / voxelSize));
	ENSURE(glm::all(glm::greaterThanEqual(originVoxels, glm::ivec3(0))));

	Layout layout;
	auto& data = layout.data;
	data.volumeOrigin[0] = origin.x;
	data.volumeOrigin[1] = origin.y;
	data.volumeOrigin[2] = origin.z;
	data.volumeOrigin[3] = voxelSize;
	data.volumeSize[0] = size.x;
	data.volumeSize[1] = size.y;
	data.volumeSize[2] = size.z;
	layout.voxelCount = size.x * size.y * size.z;
	data.voxelFirst = first.voxelCount;

	data.volumeSize[3] = 0;
	// the radiance: the frame's per voxel, then the static voxels' direct radiance
	data.radianceFirst = first.radianceCount;
	data.directFirst = first.radianceCount + layout.voxelCount;
	layout.radianceCount = 2 * layout.voxelCount;
	auto flagSize = LevelVoxels(size, SHADER_TYPES_GI_FLAG_LEVEL);
	layout.flagCount = flagSize.x * flagSize.y * flagSize.z;
	// in words (see GiFlagIndex): as blocks, the clips' flags overlapped, each clip's second half the next clip's first
	data.flagFirst = first.flagCount * SHADER_TYPES_GI_FLAG_WORD_COUNT;

	// the probes: every probeSpacing voxels from the volume's corner (which is a whole number of snaps from the anchor, so
	// they stay where they are in the world as it moves)
	glm::uvec3 probes = (size + glm::uvec3(probeSpacing - 1)) / probeSpacing;
	data.probeOrigin[0] = origin.x;
	data.probeOrigin[1] = origin.y;
	data.probeOrigin[2] = origin.z;
	data.probeOrigin[3] = static_cast<float>(probeSpacing) * voxelSize;
	data.probeSize[0] = probes.x;
	data.probeSize[1] = probes.y;
	data.probeSize[2] = probes.z;
	data.probeSize[3] = probeSpacing;
	layout.probeCount = probes.x * probes.y * probes.z;
	data.probeFirst = first.probeCount;
	return layout;
}

Clipmap MakeClipmap(
	const Bounds3f& bounds, const glm::vec3& eye, const glm::vec3& look, float lookAhead, uint32_t resolution, uint32_t clipCount, uint32_t probeSpacing)
{
	ENSURE(resolution > 0 && clipCount > 0 && clipCount <= SHADER_TYPES_GI_CLIP_COUNT);

	// the last clip: over the scene, with a margin of one and a half probe spacings around it, so that a layer of probes
	// stands outside its outermost surfaces: with a voxel of margin, the probes nearest an outer wall were in it, and its
	// outside saw none
	glm::vec3 min = bounds.GetMin();
	glm::vec3 max = bounds.GetMax();
	auto extent = glm::max(max - min, glm::vec3(1e-3F));
	uint32_t margin = (probeSpacing * 3 + 1) / 2;
	float voxelSize = std::max({extent.x, extent.y, extent.z}) / static_cast<float>(std::max(resolution, 2 * margin + 1) - 2 * margin);
	min -= glm::vec3(voxelSize * static_cast<float>(margin));
	glm::uvec3 size = glm::max(
		glm::uvec3(glm::ceil((extent + glm::vec3(2.0F * voxelSize * static_cast<float>(margin))) / voxelSize)), glm::uvec3(1));
	glm::vec3 last = glm::vec3(size) * voxelSize;
	// the finer clips are cubes of about the last clip's voxel count (a multiple of 4 voxels a side, the flags' blocks),
	// no bigger than the last clip along an axis: as slabs of the scene's shape (Bistro's 122x53x128 voxels, 14.7 m tall
	// at 27.7 cm; Sponza's 4.9 m), the ground fell out of them from a high viewpoint, and the square below the user was
	// lit by the last clip's probes, 8.9 m apart, of which the one inside the building by its entrance passed
	uint32_t cube = static_cast<uint32_t>(std::cbrt(static_cast<double>(size.x) * static_cast<double>(size.y) * static_cast<double>(size.z))) / 4 * 4;

	// the finer ones around their centers, on a grid of kSnapProbes of their probes from the last's corner (the probes stay
	// where they are in the world, see MakeLayout; what scrolls in is voxelized), inside the last. clamped to whole snaps:
	// clamped to the last's far side, a clip moved by voxels that weren't whole probe cells, so its probes' history was of
	// other places. a clip's center is ahead of the eye, toward what the camera looks at, by lookAhead of its half size
	// at most: centered on the eye, a clip gave the half of it behind the eye to what is never seen, and from a roof
	// looking at Bistro's square 27 m below, the square and the building's entrance were the last clip's (8.9 m probes),
	// which lit the square yellow by a probe inside the building; the eye stays in the finer clips (at half the
	// half size, two thirds of the way to the fade, see fadeExtent), so what is near it keeps the finest lighting.
	// the centers lie along one ray, each clip's at most half its half size from the eye, which keeps every clip inside
	// the next (the next's center is at most twice as far, its half size twice as much)
	Clipmap clipmap;
	Layout first;
	for (uint32_t clipIt = 0; clipIt < clipCount; clipIt++)
	{
		float clipVoxelSize = voxelSize / static_cast<float>(1U << (clipCount - 1 - clipIt));
		glm::uvec3 clipSize = clipIt + 1 < clipCount ? glm::min(glm::uvec3(cube), size * (1U << (clipCount - 1 - clipIt))) : size;
		glm::vec3 origin = min;
		glm::vec3 center = eye;
		if (clipIt + 1 < clipCount)
		{
			glm::vec3 clipExtent = glm::vec3(clipSize) * clipVoxelSize;
			float reach = std::min({clipExtent.x, clipExtent.y, clipExtent.z}) * 0.5F * std::clamp(lookAhead, 0.0F, 1.0F);
			float distance = glm::length(look);
			center = eye + (distance > reach ? look * (reach / distance) : look);
			float snap = clipVoxelSize * static_cast<float>(probeSpacing * kSnapProbes);
			glm::vec3 snaps = glm::floor((last - clipExtent) / snap + glm::vec3(1e-4F));
			origin = min + glm::clamp(glm::round((center - clipExtent * 0.5F - min) / snap), glm::vec3(0.0F), snaps) * snap;
		}
		auto layout = MakeLayout(origin, clipVoxelSize, clipSize, probeSpacing, first, min);
		// the box around its center it holds wherever it snapped to: half its size, less half a snap (how far from the
		// center it snaps); the shading's weight fades a point out over the probe spacing inside the clip's edges besides
		// (GiClipWeight), which the box needn't (a spacing and a voxel more of margin left Sponza's thin clips a box a
		// third of their height).
		// faded over a third of its half size: a snap then changes a point's weight by little, and smoothly as the eye moves
		auto& data = layout.data;
		data.fadeCenter[0] = center.x;
		data.fadeCenter[1] = center.y;
		data.fadeCenter[2] = center.z;
		data.fadeCenter[3] = 0.0F;
		if (clipIt + 1 < clipCount)
		{
			glm::vec3 half = glm::vec3(clipSize) * clipVoxelSize * 0.5F;
			float snap = clipVoxelSize * static_cast<float>(probeSpacing * kSnapProbes);
			glm::vec3 extent = half - glm::vec3(snap * 0.5F);
			data.fadeExtent[0] = extent.x;
			data.fadeExtent[1] = extent.y;
			data.fadeExtent[2] = extent.z;
			data.fadeExtent[3] = std::min({half.x, half.y, half.z}) / 3.0F;
		}
		else
		{
			data.fadeExtent[0] = data.fadeExtent[1] = data.fadeExtent[2] = 1e30F;
			data.fadeExtent[3] = 1.0F;
		}
		layout.data.clip = clipIt;
		layout.data.clipCount = clipCount;
		first.voxelCount += layout.voxelCount;
		first.radianceCount += layout.radianceCount;
		first.probeCount += layout.probeCount;
		first.flagCount += layout.flagCount;
		clipmap.clips.push_back(layout);
	}
	// the volumes the resolve writes hold the clips' slabs along z (see GiData::volumeImage)
	glm::uvec3 image(0);
	for (auto& clip : clipmap.clips)
	{
		image.x = std::max(image.x, clip.data.volumeSize[0]);
		image.y = std::max(image.y, clip.data.volumeSize[1]);
		clip.data.volumeImage[3] = image.z;
		image.z += clip.data.volumeSize[2];
	}
	for (auto& clip : clipmap.clips)
		for (int axisIt = 0; axisIt < 3; axisIt++)
			clip.data.volumeImage[axisIt] = image[axisIt];
	clipmap.total = first;
	return clipmap;
}

LightCells BinLights(const GiData& data, std::span<const LightData> lights)
{
	ZoneScopedN("gi::BinLights");

	constexpr uint32_t kGrid = SHADER_TYPES_GI_LIGHT_GRID;
	glm::vec3 origin(data.volumeOrigin[0], data.volumeOrigin[1], data.volumeOrigin[2]);
	glm::vec3 extent = glm::vec3(data.volumeSize[0], data.volumeSize[1], data.volumeSize[2]) * data.volumeOrigin[3];
	glm::vec3 cell = extent / static_cast<float>(kGrid);
	float farthest = glm::length(extent);

	// per cell the lights reaching it, then flattened
	std::vector<std::vector<uint32_t>> perCell(kGrid * kGrid * kGrid);
	for (uint32_t lightIt = 0; lightIt < lights.size(); lightIt++)
	{
		const auto& light = lights[lightIt];
		float range = light.positionRange[3] > 0.0F ? light.positionRange[3] : farthest;
		auto reach = LightReach(light, range);
		glm::uvec3 first(0);
		glm::uvec3 last(kGrid - 1);
		if (reach)
		{
			auto low = glm::floor((reach->center - glm::vec3(reach->radius) - origin) / cell);
			auto high = glm::floor((reach->center + glm::vec3(reach->radius) - origin) / cell);
			if (glm::any(glm::lessThan(high, glm::vec3(0.0F))) || glm::any(glm::greaterThanEqual(low, glm::vec3(kGrid))))
				continue;
			first = glm::uvec3(glm::clamp(low, glm::vec3(0.0F), glm::vec3(kGrid - 1)));
			last = glm::uvec3(glm::clamp(high, glm::vec3(0.0F), glm::vec3(kGrid - 1)));
		}
		for (uint32_t z = first.z; z <= last.z; z++)
			for (uint32_t y = first.y; y <= last.y; y++)
				for (uint32_t x = first.x; x <= last.x; x++)
					perCell[(((z * kGrid) + y) * kGrid) + x].push_back(lightIt);
	}
	LightCells cells;
	cells.ranges.reserve(perCell.size() * 2);
	for (const auto& indices : perCell)
	{
		cells.ranges.push_back(static_cast<uint32_t>(cells.indices.size()));
		cells.ranges.push_back(static_cast<uint32_t>(indices.size()));
		cells.indices.insert(cells.indices.end(), indices.begin(), indices.end());
	}
	return cells;
}

LightCells BinLights(std::span<GiData> clips, std::span<const LightData> lights)
{
	LightCells cells;
	for (auto& clip : clips)
	{
		auto clipCells = BinLights(clip, lights);
		clip.lightCellFirst = static_cast<uint32_t>(cells.ranges.size() / 2);
		auto indexFirst = static_cast<uint32_t>(cells.indices.size());
		for (size_t rangeIt = 0; rangeIt < clipCells.ranges.size(); rangeIt += 2)
		{
			cells.ranges.push_back(clipCells.ranges[rangeIt] + indexFirst);
			cells.ranges.push_back(clipCells.ranges[rangeIt + 1]);
		}
		cells.indices.insert(cells.indices.end(), clipCells.indices.begin(), clipCells.indices.end());
	}
	return cells;
}

} // namespace gfx::gi
