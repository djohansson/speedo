#pragma once

#include <rhi/enums.h>

#include <array>
#include <cstdint>
#include <string>

namespace gfx
{

// a texture as a material samples it: its file, which texcoord set (0 or 1) it reads, how that is transformed, and the
// sampler (filters and wrap modes). obj textures use set 0, untransformed, with the default sampler; gltf textures say
// (texCoord, KHR_texture_transform, the texture's sampler). the shader applies the transform (see TextureView).
struct TextureRef
{
	// the resolved path, empty if the material has no such texture. a texture that can't be found is counted in
	// mesh::Stats::missingTextures and left empty.
	std::string path;
	uint32_t texCoord = 0;
	// (u', v') = (transform[0] * u + transform[1] * v + transform[2], transform[3] * u + transform[4] * v + transform[5])
	std::array<float, 6> transform{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
	rhi::SamplerDesc sampler{.maxAnisotropy = kDefaultMaxAnisotropy};

	static constexpr float kDefaultMaxAnisotropy = 16.0F;

	[[nodiscard]] bool empty() const noexcept { return path.empty(); }
};

} // namespace gfx
