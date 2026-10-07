#pragma once

#include <rhi/enums.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
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
	// gltf: an image the file at path (a model) embeds, by its index, which the texture loader reads from it (see
	// gltf::EmbeddedImage), rather than an image file
	std::optional<uint32_t> embeddedImage;
	uint32_t texCoord = 0;
	// (u', v') = (transform[0] * u + transform[1] * v + transform[2], transform[3] * u + transform[4] * v + transform[5])
	std::array<float, 6> transform{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
	rhi::SamplerDesc sampler{.maxAnisotropy = kDefaultMaxAnisotropy};
	// if an animation moves the transform (KHR_animation_pointer): the index of its offset's ScenePointerTarget, which
	// its rotation's and scale's follow (see TextureTransform). -1 if none does.
	int32_t animatedTransform = -1;

	static constexpr float kDefaultMaxAnisotropy = 16.0F;

	[[nodiscard]] bool empty() const noexcept { return path.empty(); }
};

// KHR_texture_transform's offset, rotation (radians) and scale as TextureRef::transform: translation * rotation * scale
[[nodiscard]] inline std::array<float, 6> TextureTransform(const std::array<float, 2>& offset, float rotation, const std::array<float, 2>& scale)
{
	auto c = std::cos(rotation);
	auto s = std::sin(rotation);
	return {c * scale[0], s * scale[1], offset[0], -s * scale[0], c * scale[1], offset[1]};
}

} // namespace gfx
