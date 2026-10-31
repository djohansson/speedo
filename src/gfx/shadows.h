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
constexpr uint32_t kCellSize = 128; // the atlas is a grid of these: the smallest tile
constexpr uint32_t kCellsPerRow = kAtlasSize / kCellSize;
// the tiles' sizes, in cells (powers of two): a cascade's 8 (1024 texels), at most; a spot light's and a point light's
// faces by how big the light's range looks from the camera, at most 8 (a spot's 1024 texels) and 4 (a face's 512), at
// least one cell. a light whose tiles don't fit gets them halved until they do (all of a light's tiles or none)
constexpr uint32_t kCascadeCells = 8;
constexpr uint32_t kSpotCells = 8;
constexpr uint32_t kPointFaceCells = 4;

// what the shadows are fitted to: each view's camera (split screen's, each with its cascades), and the scene's bounds
// (what casts shadows)
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

// plans the shadows of lights (as the shader sees them) for the views' cameras and the scene's bounds. lights beyond the
// budget (the atlas is full, or SHADER_TYPES_SHADOW_VIEW_COUNT views) cast none: directional lights go first (by
// intensity), then the others by how bright they are at the nearest eye. a directional light has cascades fitted to each
// camera, all of them finest first (SHADER_TYPES_SHADOW_CASCADE_COUNT times the cameras: the shader takes the first that
// covers a point, the finest any view has, see PlaceInShadowMap). point and spot lights whose range is out of every
// camera's view light nothing seen, and get none; the others are sized for the view they look biggest in
[[nodiscard]] ShadowPlan Plan(std::span<const LightData> lights, std::span<const ShadowCamera> cameras, const Bounds3f& sceneBounds);

} // namespace gfx::shadows
