#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace gfx::image
{

enum class Format : uint8_t
{
	kBC1, // rgb, 8 bytes per 4x4 block
	kBC3, // rgba, 16 bytes per 4x4 block
	kBC4, // r, 8 bytes per 4x4 block
	kBC5, // rg, 16 bytes per 4x4 block
};

// what an image is used for, which decides how it is filtered and compressed
enum class Usage : uint8_t
{
	kColor, // srgb color (and alpha): BC1, or BC3 if any pixel isn't opaque. mips are filtered in linear space.
	kLinear, // anything else, as is: BC1 or BC3
	// a tangent space normal map, in the usual (OpenGL, +y up the image) convention: BC5 holding x and y (z is
	// reconstructed). stored with +x along +u and +y down the image, along +v as sampled (the obj importer flips v),
	// i.e. with y flipped.
	kNormal,
	kMask, // one channel, e.g. an alpha mask: the alpha channel if any pixel isn't opaque, else the luminance. BC4.
	// a bump texture: a height map, which is turned into a normal map, or (told apart by color) a normal map. as kNormal.
	kBump,
	// an ambient occlusion map: its red channel (gltf packs occlusion, roughness and metallic in r, g and b), linear.
	// BC4. (appended: usages are part of the serialized images)
	kOcclusion,
	// a gltf metallic-roughness texture: roughness (its green channel) in r and metallic (blue) in g, linear. BC5.
	kMetallicRoughness,
};

struct Options
{
	Usage usage = Usage::kColor;
	// for kBump, if the image is a height map: how deep its full range is, in 1/64ths of the image's width (the mtl -bm
	// option)
	float bumpScale = 1.0F;
};

struct MipLevel
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t offset = 0; // in bytes, from the start of the image data
	uint32_t size = 0; // in bytes
};

struct Image
{
	uint32_t channelCount = 0; // in the file
	Format format = Format::kBC1;
	Usage usage = Usage::kColor;
	bool fromHeight = false; // for kBump: the file was a height map
	std::vector<MipLevel> mipLevels; // the full chain, down to 1x1
	size_t size = 0; // in bytes, of all mip levels
};

// level 0 of an image prepared for a usage, before compression: rgba, with for kNormal and kBump the normal * 0.5 + 0.5
// in rgb, and for kMask the mask in rgb
struct Pixels
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t channelCount = 0; // in the file
	bool alpha = false; // for kColor and kLinear: some pixel isn't opaque
	bool fromHeight = false; // for kBump: the file was a height map
	std::vector<uint8_t> rgba;
};

[[nodiscard]] constexpr uint32_t BlockSize(Format format) noexcept
{
	return format == Format::kBC1 || format == Format::kBC4 ? 8 : 16; //NOLINT(readability-magic-numbers)
}

// decodes an image file (anything stb_image reads) and prepares it for a usage, as Import does first. for testing.
[[nodiscard]] std::expected<Pixels, std::string> Decode(const std::filesystem::path& path, const Options& options);

// decodes an image file, prepares it for a usage, generates its mip chain and compresses it. calls allocate once with
// the size of the compressed data, which it writes to the returned memory, mip level 0 first. progress is advanced
// from its current value to 224 while compressing. returns an error message if the file can't be read or decoded, or if
// cancelled() returns true (allocate may have been called then).
[[nodiscard]] std::expected<Image, std::string> Import(
	const std::filesystem::path& path,
	const Options& options,
	const std::function<std::byte*(size_t size)>& allocate,
	std::atomic_uint8_t* progress = nullptr,
	const std::function<bool()>& cancelled = {});

// decompresses one block of format to 4x4 rgba pixels, row by row: bc4 to the value in rgb, bc5 to x, y and the
// reconstructed z (each * 0.5 + 0.5) in rgb, both with alpha 255. for testing.
void DecompressBlock(Format format, std::span<const std::byte> block, std::span<uint8_t, 64> rgbaOut) noexcept;

} // namespace gfx::image
