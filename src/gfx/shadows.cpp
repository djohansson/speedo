#include "shadows.h"

#include <core/profiling.h>

#include <gfx/frustum.h>
#include <gfx/lightbounds.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numeric>
#include <optional>

namespace gfx::shadows
{

namespace detail
{

// gl's clip space to vulkan's, as the cameras' (see Camera::UpdateProjectionMatrix): y down, depth 0 to 1. it reverses
// the winding, so the shadow pass culls the faces the main pass does
const glm::mat4 kClip{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.5F, 1.0F};

// the share of the cascades' splits that is logarithmic (the rest uniform): Zhang's practical split scheme
constexpr float kSplitLambda = 0.75F;

[[nodiscard]] float Luminance(const float* rgb)
{
	return (0.2126F * rgb[0]) + (0.7152F * rgb[1]) + (0.0722F * rgb[2]);
}

[[nodiscard]] std::array<glm::vec3, 8> Corners(const Bounds3f& bounds)
{
	std::array<glm::vec3, 8> corners;
	for (size_t i = 0; i < corners.size(); i++)
		corners[i] = glm::vec3(
			(i & 1U) != 0 ? bounds.GetMax().x : bounds.GetMin().x,
			(i & 2U) != 0 ? bounds.GetMax().y : bounds.GetMin().y,
			(i & 4U) != 0 ? bounds.GetMax().z : bounds.GetMin().z);
	return corners;
}

// an up vector for a view looking along forward (not parallel to it)
[[nodiscard]] glm::vec3 UpFor(const glm::vec3& forward)
{
	return std::abs(forward.y) < 0.99F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F);
}

void Store(ShadowData& data, const glm::mat4& viewProjection)
{
	std::copy_n(&viewProjection[0][0], 16, &data.viewProjection[0][0]); //NOLINT(readability-magic-numbers)
}

// the atlas's cells, taken by square tiles
class Allocator
{
public:
	// a free square of size cells (aligned to its size), or none
	[[nodiscard]] std::optional<ShadowPlan::Tile> Allocate(uint32_t size)
	{
		for (uint32_t y = 0; y + size <= kCellsPerRow; y += size)
			for (uint32_t x = 0; x + size <= kCellsPerRow; x += size)
				if (Free(x, y, size))
				{
					Mark(x, y, size, true);
					return ShadowPlan::Tile{.x = x * kCellSize, .y = y * kCellSize, .size = size * kCellSize};
				}
		return std::nullopt;
	}
	void Release(const ShadowPlan::Tile& tile) { Mark(tile.x / kCellSize, tile.y / kCellSize, tile.size / kCellSize, false); }

private:
	[[nodiscard]] bool Free(uint32_t x, uint32_t y, uint32_t size) const
	{
		for (uint32_t j = y; j < y + size; j++)
			for (uint32_t i = x; i < x + size; i++)
				if (myTaken[(j * kCellsPerRow) + i])
					return false;
		return true;
	}
	void Mark(uint32_t x, uint32_t y, uint32_t size, bool taken)
	{
		for (uint32_t j = y; j < y + size; j++)
			for (uint32_t i = x; i < x + size; i++)
				myTaken[(j * kCellsPerRow) + i] = taken;
	}

	std::array<bool, kCellsPerRow * kCellsPerRow> myTaken{};
};

// the tiles of a light, all or none: count squares of cells cells, else of half that, down to one cell
[[nodiscard]] std::optional<std::vector<ShadowPlan::Tile>> AllocateAll(Allocator& allocator, uint32_t count, uint32_t cells)
{
	for (; cells >= 1; cells /= 2)
	{
		std::vector<ShadowPlan::Tile> tiles;
		for (uint32_t i = 0; i < count; i++)
		{
			auto tile = allocator.Allocate(cells);
			if (!tile)
				break;
			tiles.push_back(*tile);
		}
		if (tiles.size() == count)
			return tiles;
		for (const auto& taken : tiles)
			allocator.Release(taken);
	}
	return std::nullopt;
}

// the cells a tile of a light whose range (a sphere at position, of radius reach) looks size big from the camera
// needs, at most maxCells: a power of two, by its share of the view's height (a light filling the view gets maxCells,
// one a quarter of it a quarter of that)
[[nodiscard]] uint32_t CellsFor(const glm::mat4& projection, const glm::mat4& view, const glm::vec3& position, float reach, uint32_t maxCells)
{
	float distance = -(view * glm::vec4(position, 1.0F)).z;
	bool orthographic = projection[2][3] == 0.0F;
	// the sphere's radius over the view's half height there (see the projection's y scale)
	float share = reach * std::abs(projection[1][1]) / (orthographic ? 1.0F : std::max(distance, 1e-4F));
	if (!orthographic && distance <= reach)
		share = 1.0F; // the camera is in the light's range
	auto cells = static_cast<float>(maxCells) * std::min(share, 1.0F);
	return std::clamp(std::bit_floor(static_cast<uint32_t>(std::max(cells, 1.0F))), 1U, maxCells);
}

[[nodiscard]] glm::vec4 AtlasRect(const ShadowPlan::Tile& tile)
{
	auto scale = 1.0F / static_cast<float>(kAtlasSize);
	return {static_cast<float>(tile.x) * scale, static_cast<float>(tile.y) * scale, static_cast<float>(tile.size) * scale, static_cast<float>(tile.size) * scale};
}

// a directional light's cascades: the camera's frustum, up to the scene's farthest point, in slices (practical splits),
// each in a sphere (so that the cascade's size doesn't change as the camera turns) seen along the light, from wherever
// the scene's casters are, and moved by whole texels only (so that its texels don't crawl as the camera moves)
void Cascades(
	const glm::vec3& direction,
	const ShadowCamera& camera,
	const std::array<glm::vec3, 8>& sceneCorners,
	std::span<const ShadowPlan::Tile> tiles,
	std::vector<ShadowData>& views)
{
	std::array<glm::vec3, 4> nearCorners;
	std::array<glm::vec3, 4> farCorners;
	for (size_t i = 0; i < 4; i++)
	{
		glm::vec2 ndc((i & 1U) != 0 ? 1.0F : -1.0F, (i & 2U) != 0 ? 1.0F : -1.0F);
		auto nearPoint = camera.inverseViewProjection * glm::vec4(ndc, 0.0F, 1.0F);
		auto farPoint = camera.inverseViewProjection * glm::vec4(ndc, 1.0F, 1.0F);
		nearCorners[i] = glm::vec3(nearPoint) / nearPoint.w;
		farCorners[i] = glm::vec3(farPoint) / farPoint.w;
	}

	// as far as the scene goes (along the view), within the camera's range
	float sceneDepth = 0.0F;
	for (const auto& corner : sceneCorners)
		sceneDepth = std::max(sceneDepth, -(camera.view * glm::vec4(corner, 1.0F)).z);
	float nearDepth = camera.nearPlane;
	float farDepth = std::clamp(sceneDepth, nearDepth * 2.0F, camera.farPlane);

	std::array<float, SHADER_TYPES_SHADOW_CASCADE_COUNT + 1> splits{};
	for (uint32_t i = 0; i <= SHADER_TYPES_SHADOW_CASCADE_COUNT; i++)
	{
		float t = static_cast<float>(i) / static_cast<float>(SHADER_TYPES_SHADOW_CASCADE_COUNT);
		float logarithmic = nearDepth * std::pow(farDepth / nearDepth, t);
		float uniform = nearDepth + ((farDepth - nearDepth) * t);
		splits[i] = (kSplitLambda * logarithmic) + ((1.0F - kSplitLambda) * uniform);
	}

	auto up = UpFor(direction);
	for (uint32_t cascade = 0; cascade < SHADER_TYPES_SHADOW_CASCADE_COUNT; cascade++)
	{
		// the slice's corners: along the frustum's edges (straight lines, for perspective and orthographic views alike)
		std::array<glm::vec3, 8> slice;
		for (size_t i = 0; i < 4; i++)
			for (size_t end = 0; end < 2; end++)
			{
				float t = (splits[cascade + end] - camera.nearPlane) / (camera.farPlane - camera.nearPlane);
				slice[(end * 4) + i] = glm::mix(nearCorners[i], farCorners[i], t);
			}
		glm::vec3 center = std::accumulate(slice.begin(), slice.end(), glm::vec3(0.0F)) / 8.0F;
		float radius = 0.0F;
		for (const auto& corner : slice)
			radius = std::max(radius, glm::distance(corner, center));
		// in steps of a 16th of itself, so that it doesn't change by rounding errors from frame to frame
		float step = std::exp2(std::floor(std::log2(std::max(radius, 1e-6F))) - 4.0F);
		radius = std::ceil(radius / step) * step;

		// depth: from the nearest caster in the scene (or the slice) to the farthest point
		auto lightView = glm::lookAt(center, center + direction, up);
		float nearest = -radius;
		float farthest = radius;
		for (const auto& corner : sceneCorners)
		{
			float along = glm::dot(corner - center, direction);
			nearest = std::min(nearest, along);
			farthest = std::max(farthest, along);
		}
		float margin = (farthest - nearest) * 0.01F;
		auto projection = glm::ortho(-radius, radius, -radius, radius, nearest - margin, farthest + margin);
		auto viewProjection = kClip * projection * lightView;

		// whole texels: the world origin lands on a texel corner
		const auto& tile = tiles[cascade];
		float texels = static_cast<float>(tile.size);
		auto origin = viewProjection * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
		glm::vec2 snapped = glm::round(glm::vec2(origin) * texels * 0.5F) / (texels * 0.5F);
		auto snap = glm::translate(glm::mat4(1.0F), glm::vec3(snapped - glm::vec2(origin), 0.0F));
		viewProjection = snap * viewProjection;

		ShadowData data{};
		Store(data, viewProjection);
		auto rect = AtlasRect(tile);
		std::copy_n(&rect[0], 4, data.atlasRect);
		data.params[0] = 2.0F * radius / texels;
		data.params[1] = 0.0F;
		views.push_back(data);
	}
}

// a perspective view of a light at position along forward, fovY wide, to far, in a tile
ShadowData PerspectiveView(
	const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, float fovY, float farPlane, const ShadowPlan::Tile& tile)
{
	float nearPlane = std::max(farPlane * 1e-3F, 1e-4F);
	auto viewProjection = kClip * glm::perspective(fovY, 1.0F, nearPlane, farPlane) * glm::lookAt(position, position + forward, up);
	ShadowData data{};
	Store(data, viewProjection);
	auto rect = AtlasRect(tile);
	std::copy_n(&rect[0], 4, data.atlasRect);
	data.params[0] = 2.0F * std::tan(fovY * 0.5F) / static_cast<float>(tile.size);
	data.params[1] = 1.0F;
	return data;
}

} // namespace detail

ShadowPlan Plan(std::span<const LightData> lights, const ShadowCamera& camera, const Bounds3f& sceneBounds)
{
	using namespace detail;

	ZoneScopedN("gfx::shadows::Plan");

	ShadowPlan plan;
	plan.lightShadows.assign(lights.size(), SHADER_TYPES_NO_SHADOW);
	// nothing to cast shadows: no scene (an empty or inverted box)
	const auto& min = sceneBounds.GetMin();
	const auto& max = sceneBounds.GetMax();
	if (lights.empty() || !glm::all(glm::lessThanEqual(min, max)) || !glm::any(glm::lessThan(min, max)))
		return plan;

	auto sceneCorners = Corners(sceneBounds);
	auto eye = glm::vec3(glm::inverse(camera.view)[3]);

	// by priority: directional lights by intensity, then the others by their intensity at the camera
	std::vector<size_t> order(lights.size());
	std::iota(order.begin(), order.end(), 0);
	auto priority = [&](size_t index)
	{
		const auto& light = lights[index];
		float luminance = Luminance(light.intensity);
		if (light.type == LIGHT_TYPE_DIRECTIONAL)
			return 1e30F + luminance;
		glm::vec3 position(light.positionRange[0], light.positionRange[1], light.positionRange[2]);
		return luminance / std::max(glm::dot(position - eye, position - eye), 1e-4F);
	};
	std::ranges::stable_sort(order, [&](size_t a, size_t b) { return priority(a) > priority(b); });

	// what the camera sees: a light whose range is outside it lights nothing seen
	auto viewProjection = glm::inverse(camera.inverseViewProjection);
	auto projection = viewProjection * glm::inverse(camera.view);
	auto frustum = Frustum::FromViewProjection(viewProjection);

	// the candidates, by priority: each light's views and the size of their tiles (in cells) it would like
	struct Candidate
	{
		size_t index = 0;
		glm::vec3 position{};
		glm::vec3 direction{};
		float reach = 0.0F;
		uint32_t tileCount = 0;
		uint32_t cells = 0;
	};
	std::vector<Candidate> candidates;
	uint32_t viewTotal = 0;
	for (auto index : order)
	{
		const auto& light = lights[index];
		if (Luminance(light.intensity) <= 0.0F)
			continue;
		uint32_t tileCount = light.type == LIGHT_TYPE_DIRECTIONAL ? SHADER_TYPES_SHADOW_CASCADE_COUNT : light.type == LIGHT_TYPE_POINT ? 6 : 1;
		if (viewTotal + tileCount > SHADER_TYPES_SHADOW_VIEW_COUNT)
			continue;

		glm::vec3 position(light.positionRange[0], light.positionRange[1], light.positionRange[2]);
		glm::vec3 direction = glm::normalize(glm::vec3(light.direction[0], light.direction[1], light.direction[2]));
		// to the scene's farthest corner, unless the light's range ends sooner
		float reach = 0.0F;
		for (const auto& corner : sceneCorners)
			reach = std::max(reach, glm::distance(corner, position));
		if (light.positionRange[3] > 0.0F)
			reach = std::min(reach, light.positionRange[3]);
		uint32_t cells = kCascadeCells;
		if (light.type != LIGHT_TYPE_DIRECTIONAL)
		{
			if (reach <= 0.0F)
				continue;
			// what it can light (a spot light's cone, not its whole range): out of view, it lights nothing seen
			auto sphere = *LightReach(light, reach);
			Bounds3f range;
			range.SetMin(sphere.center - glm::vec3(sphere.radius));
			range.SetMax(sphere.center + glm::vec3(sphere.radius));
			if (frustum.Excludes(range))
				continue;
			cells = CellsFor(
				projection, camera.view, sphere.center, sphere.radius, light.type == LIGHT_TYPE_POINT ? kPointFaceCells : kSpotCells);
		}
		candidates.push_back({index, position, direction, reach, tileCount, cells});
		viewTotal += tileCount;
	}

	// the budget: while they need more cells than the atlas has, the lowest priority light of those with the biggest
	// tiles gets them halved (the cascades keep theirs), and once all are one cell, the lowest priority light goes
	auto cellTotal = [&candidates]
	{
		uint32_t total = 0;
		for (const auto& candidate : candidates)
			total += candidate.tileCount * candidate.cells * candidate.cells;
		return total;
	};
	auto local = [&lights](const Candidate& candidate) { return lights[candidate.index].type != LIGHT_TYPE_DIRECTIONAL; };
	while (!candidates.empty() && cellTotal() > kCellsPerRow * kCellsPerRow)
	{
		uint32_t biggest = 0;
		for (const auto& candidate : candidates)
			if (local(candidate))
				biggest = std::max(biggest, candidate.cells);
		if (biggest > 1)
		{
			auto it = std::find_if(
				candidates.rbegin(), candidates.rend(), [&](const Candidate& candidate) { return local(candidate) && candidate.cells == biggest; });
			it->cells /= 2;
		}
		else
			candidates.pop_back();
	}

	// the tiles, biggest first: aligned squares of powers of two then pack the atlas without gaps
	std::vector<size_t> bySize(candidates.size());
	std::iota(bySize.begin(), bySize.end(), 0);
	std::ranges::stable_sort(bySize, [&candidates](size_t a, size_t b) { return candidates[a].cells > candidates[b].cells; });
	std::vector<std::optional<std::vector<ShadowPlan::Tile>>> tiles(candidates.size());
	Allocator allocator;
	for (auto candidateIt : bySize)
		tiles[candidateIt] = AllocateAll(allocator, candidates[candidateIt].tileCount, candidates[candidateIt].cells);

	// the views, by priority
	for (size_t candidateIt = 0; candidateIt < candidates.size(); candidateIt++)
	{
		if (!tiles[candidateIt])
			continue;
		const auto& [index, position, direction, reach, tileCount, cells] = candidates[candidateIt];
		const auto& light = lights[index];
		const auto& lightTiles = *tiles[candidateIt];
		auto first = static_cast<uint32_t>(plan.views.size());
		if (light.type == LIGHT_TYPE_DIRECTIONAL)
		{
			Cascades(direction, camera, sceneCorners, lightTiles, plan.views);
		}
		else if (light.type == LIGHT_TYPE_POINT)
		{
			// each face a little wider than 90 degrees, so that the filter of a point near a face's edge stays inside it
			float faceTexels = static_cast<float>(lightTiles.front().size);
			float fov = 2.0F * std::atan(1.0F + (8.0F / faceTexels));
			static const std::array<std::pair<glm::vec3, glm::vec3>, 6> kFaces{{
				{{1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}},
				{{-1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}},
				{{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
				{{0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}},
				{{0.0F, 0.0F, 1.0F}, {0.0F, -1.0F, 0.0F}},
				{{0.0F, 0.0F, -1.0F}, {0.0F, -1.0F, 0.0F}},
			}};
			for (size_t face = 0; face < kFaces.size(); face++)
				plan.views.push_back(PerspectiveView(position, kFaces[face].first, kFaces[face].second, fov, reach, lightTiles[face]));
		}
		else
		{
			// the cone (its outer angle: the attenuation's 0, see LightData::spotOffset), a little wider for the filter
			float outer = std::acos(SpotCosOuter(light));
			float fov = std::min((2.0F * outer) + glm::radians(4.0F), glm::radians(170.0F));
			plan.views.push_back(PerspectiveView(position, direction, UpFor(direction), fov, reach, lightTiles.front()));
		}
		plan.tiles.insert(plan.tiles.end(), lightTiles.begin(), lightTiles.end());
		plan.lightShadows[index] = first;
	}

	return plan;
}

} // namespace gfx::shadows
