#pragma once

#include <gfx/bounds.h>
#include <gfx/shaders/capi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

// shadows of the punctual lights, in one depth texture (the atlas) of square tiles, planned on the cpu each frame:
// which lights get them (a budget: the tiles the atlas holds, by priority), and the views they are drawn from.
// directional lights get SHADER_TYPES_SHADOW_CASCADE_COUNT cascades fitted to the camera, point lights a cube of 6
// faces, spot lights one perspective view around their cone.
namespace gfx::shadows
{

constexpr uint32_t kAtlasSize = 4096; // texels, square
constexpr uint32_t kCellSize = 512; // the atlas is a grid of these
constexpr uint32_t kCellsPerRow = kAtlasSize / kCellSize;
constexpr uint32_t kCascadeCells = 2; // a cascade's tile is 2x2 cells (1024 texels), as is a spot light's
constexpr uint32_t kSpotCells = 2;
constexpr uint32_t kPointFaceCells = 1; // a point light's faces 512 texels each
// at most this many point and spot lights cast shadows (the nearest and brightest): each of their views draws the whole
// scene (there is no culling per view)
constexpr uint32_t kMaxLocalShadows = 4;

// what the shadows are fitted to: the camera (the first view), and the scene's bounds (what casts shadows)
struct ShadowCamera
{
	glm::mat4 view{1.0F};
	glm::mat4 inverseViewProjection{1.0F};
	float nearPlane = 0.01F;
	float farPlane = 100.0F;
};

// a frame's shadows: the views (gShadowViews), each light's first (gLightShadows), and the pixel rectangles of the views'
// tiles in the atlas, to draw them in
struct ShadowPlan
{
	std::vector<ShadowData> views;
	std::vector<uint32_t> lightShadows; // per light: its first view, or SHADER_TYPES_NO_SHADOW
	struct Tile
	{
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t size = 0;
	};
	std::vector<Tile> tiles; // per view
};

// plans the shadows of lights (as the shader sees them) for a camera and the scene's bounds. lights beyond the budget
// (the atlas is full) cast none: directional lights go first (by intensity), then the others by how bright they are
// at the camera.
[[nodiscard]] ShadowPlan Plan(std::span<const LightData> lights, const ShadowCamera& camera, const Bounds3f& sceneBounds);

} // namespace gfx::shadows
