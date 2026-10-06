#pragma once

#include <array>
#include <string>

namespace gfx
{

// a camera a model file defines (gltf cameras), in world space: what the views can be set to (see Views::SetScene)
struct SceneCamera
{
	std::string name;
	std::array<float, 3> position{};
	std::array<float, 3> forward{0.0F, 0.0F, -1.0F}; // unit length, where it looks
	std::array<float, 3> up{0.0F, 1.0F, 0.0F}; // unit length: its roll about forward
	bool orthographic = false;
	float yfov = 0.0F; // perspective: the vertical field of view, in radians
	float ymag = 0.0F; // orthographic: half the view's height. its width follows the view's aspect ratio
	float znear = 0.0F;
	float zfar = 0.0F; // 0: none given (gltf perspective cameras may be infinite), the views fit it to the bounds
	// width / height (gltf aspectRatio, or xmag / ymag): the views letterbox to it. 0: none given, the view's own
	float aspectRatio = 0.0F;
};

} // namespace gfx
