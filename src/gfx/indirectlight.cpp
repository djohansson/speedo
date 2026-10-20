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
	const glm::vec3& origin,
	float voxelSize,
	const glm::uvec3& size,
	uint32_t probeSpacing,
	uint32_t octahedralSide,
	float reach,
	const Layout& first)
{
	ENSURE(probeSpacing > 0 && octahedralSide > 0 && voxelSize > 0.0F);

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

	// the levels, down to a voxel or SHADER_TYPES_GI_LEVEL_COUNT
	uint32_t levelCount = std::min<uint32_t>(std::bit_width(std::max({size.x, size.y, size.z})), SHADER_TYPES_GI_LEVEL_COUNT);
	data.volumeSize[3] = levelCount;
	for (uint32_t levelIt = 0; levelIt < levelCount; levelIt++)
	{
		data.levelFirst[levelIt / 4][levelIt % 4] = first.radianceCount + layout.radianceCount;
		auto levelSize = glm::max(size >> levelIt, glm::uvec3(1));
		layout.radianceCount += levelSize.x * levelSize.y * levelSize.z;
	}
	data.directFirst = first.radianceCount + layout.radianceCount;
	layout.radianceCount += layout.voxelCount;

	// the cascades: each with half the probes per axis, twice the directions per axis and an interval four times as long,
	// until one reaches reach. cascade 0's interval ends at twice its probes' spacing: ending at the spacing,
	// the probes above (twice as far apart) are as far from a probe as its rays reach, and the merge's parallax lost a
	// third of the light on Sponza's outer wall against the reference (67% of it, 86% now; the bilinear fix, a ray from
	// each probe to each of the 8 above, lost more: its bridging rays leave the direction by up to 60 degrees there)
	constexpr float kIntervalScale = 2.0F;
	float spacing0 = static_cast<float>(probeSpacing) * voxelSize;
	glm::uvec3 probes0 = (size + glm::uvec3(probeSpacing - 1)) / probeSpacing;
	uint32_t cascadeCount = 0;
	for (uint32_t cascadeIt = 0; cascadeIt < SHADER_TYPES_GI_CASCADE_COUNT; cascadeIt++)
	{
		auto probes = glm::max((probes0 + glm::uvec3((1U << cascadeIt) - 1)) >> cascadeIt, glm::uvec3(1));
		uint32_t side = octahedralSide << cascadeIt;
		float scale = static_cast<float>(1U << (2 * cascadeIt));
		float start = kIntervalScale * spacing0 * (scale - 1.0F) / 3.0F;
		float end = kIntervalScale * spacing0 * ((scale * 4.0F) - 1.0F) / 3.0F;
		data.cascadeSize[cascadeIt][0] = probes.x;
		data.cascadeSize[cascadeIt][1] = probes.y;
		data.cascadeSize[cascadeIt][2] = probes.z;
		data.cascadeSize[cascadeIt][3] = side;
		data.cascadeInterval[cascadeIt][0] = start;
		data.cascadeInterval[cascadeIt][1] = end;
		data.cascadeInterval[cascadeIt][2] = 0.0F;
		data.cascadeInterval[cascadeIt][3] = spacing0 * static_cast<float>(1U << cascadeIt);
		data.cascadeFirst[cascadeIt / 4][cascadeIt % 4] = first.cascadeTexelCount + layout.cascadeTexelCount;
		data.cascadeProbeFirst[cascadeIt / 4][cascadeIt % 4] = first.probeMaskCount + layout.probeMaskCount;
		layout.cascadeTexelCount += probes.x * probes.y * probes.z * side * side;
		layout.probeMaskCount += probes.x * probes.y * probes.z;
		cascadeCount++;
		if (end >= reach)
			break;
	}
	data.cascadeCount = cascadeCount;
	layout.probeCount = probes0.x * probes0.y * probes0.z;
	data.probeFirst = first.probeCount;
	return layout;
}

Clipmap MakeClipmap(
	const Bounds3f& bounds,
	const glm::vec3& eye,
	uint32_t resolution,
	uint32_t clipCount,
	uint32_t probeSpacing,
	uint32_t octahedralSide)
{
	ENSURE(resolution > 0 && clipCount > 0 && clipCount <= SHADER_TYPES_GI_CLIP_COUNT);

	// the last clip: over the scene, with a margin of a voxel around it, so that its surfaces aren't on the volume's edge
	glm::vec3 min = bounds.GetMin();
	glm::vec3 max = bounds.GetMax();
	auto extent = glm::max(max - min, glm::vec3(1e-3F));
	float voxelSize = std::max({extent.x, extent.y, extent.z}) / static_cast<float>(std::max(resolution, 3U) - 2);
	min -= glm::vec3(voxelSize);
	glm::uvec3 size = glm::max(glm::uvec3(glm::ceil((extent + glm::vec3(2.0F * voxelSize)) / voxelSize)), glm::uvec3(1));
	glm::vec3 last = glm::vec3(size) * voxelSize;
	float reach = glm::length(last);

	// the finer ones around the eye, on a grid of 4 of their cascade 0 probes (what moves when they do: their cascades
	// above that are a frame's probes of another place, and their voxels are made again), inside the last
	Clipmap clipmap;
	Layout first;
	for (uint32_t clipIt = 0; clipIt < clipCount; clipIt++)
	{
		float clipVoxelSize = voxelSize / static_cast<float>(1U << (clipCount - 1 - clipIt));
		glm::vec3 origin = min;
		if (clipIt + 1 < clipCount)
		{
			glm::vec3 clipExtent = glm::vec3(size) * clipVoxelSize;
			float snap = clipVoxelSize * static_cast<float>(probeSpacing * 4);
			origin = min + glm::round((eye - clipExtent * 0.5F - min) / snap) * snap;
			origin = glm::clamp(origin, min, min + last - clipExtent);
		}
		auto layout = MakeLayout(origin, clipVoxelSize, size, probeSpacing, octahedralSide, reach, first);
		layout.data.clip = clipIt;
		layout.data.clipCount = clipCount;
		first.voxelCount += layout.voxelCount;
		first.radianceCount += layout.radianceCount;
		first.cascadeTexelCount += layout.cascadeTexelCount;
		first.probeCount += layout.probeCount;
		first.probeMaskCount += layout.probeMaskCount;
		clipmap.clips.push_back(layout);
	}
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
