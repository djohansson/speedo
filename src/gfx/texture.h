#pragma once

#include <gfx/gpu.h>
#include <gfx/imageimport.h>
#include <gfx/upload.h>

#include <array>
#include <atomic>
#include <optional>
#include <cstdint>
#include <memory>
#include <string_view>

namespace gfx
{

// an image uploaded for sampling, and a view of it
struct Texture
{
	std::shared_ptr<Image> image;
	std::shared_ptr<ImageView> view;
	Upload upload; // of the image, which gpu work that uses it must wait for and acquire it from
	bool normalYUp = false; // see image::Image::normalYUp

	[[nodiscard]] explicit operator bool() const noexcept { return image != nullptr; }
};

// loads an image file through the asset cache (see core::file::LoadAsset), prepared for options.usage (see
// image::Import: color images get an srgb format, so that sampling them returns linear values), and uploads it on the
// primary device's transfer queue. returns once the upload is submitted (see Texture::upload); the image is left in the
// layout of the upload, so the caller must transition it before sampling from it. returns an empty texture if the load was cancelled because the
// application is exiting, or failed (the reason is printed to stderr).
// embeddedImage: an image the gltf file at filePath embeds (see TextureRef::embeddedImage), else filePath is the image
[[nodiscard]] Texture LoadTexture(
	std::string_view filePath,
	std::atomic_uint8_t& progress,
	const image::Options& options = {},
	std::optional<uint32_t> embeddedImage = std::nullopt);

// a prefiltered environment uploaded for sampling (see environment::Environment): its levels as the mips of a
// R16G16B16A16_SFLOAT texture, its sheen levels as another's, and its irradiance
struct EnvironmentTexture
{
	Texture texture;
	Texture sheenTexture;
	uint32_t sheenLevelCount = 0;
	std::array<std::array<float, 4>, 9> irradiance{};
	uint32_t levelCount = 0;

	[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(texture); }
};

// loads an environment panorama file (e.g. Radiance .hdr) through the asset cache, or the procedural sky (see
// environment::ProceduralSky) without one, prefilters it and uploads it as LoadTexture does. empty if cancelled or failed.
[[nodiscard]] EnvironmentTexture LoadEnvironment(std::optional<std::string_view> filePath, std::atomic_uint8_t& progress);

} // namespace gfx
