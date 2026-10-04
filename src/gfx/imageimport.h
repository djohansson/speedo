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
	Format format = Format::kBC1; // kBC3 if any pixel isn't opaque
	std::vector<MipLevel> mipLevels; // the full chain, down to 1x1
	size_t size = 0; // in bytes, of all mip levels
};

[[nodiscard]] constexpr uint32_t BlockSize(Format format) noexcept { return format == Format::kBC1 ? 8 : 16; }

// decodes an image file (anything stb_image reads), generates its mip chain and compresses it. calls allocate once
// with the size of the compressed data, which it writes to the returned memory, mip level 0 first. progress is advanced
// from its current value to 224 while compressing. returns an error message if the file can't be read or decoded, or if
// cancelled() returns true (allocate may have been called then).
[[nodiscard]] std::expected<Image, std::string> Import(
	const std::filesystem::path& path,
	const std::function<std::byte*(size_t size)>& allocate,
	std::atomic_uint8_t* progress = nullptr,
	const std::function<bool()>& cancelled = {});

// decompresses one block of format to 4x4 rgba pixels, row by row. for testing.
void DecompressBlock(Format format, std::span<const std::byte> block, std::span<uint8_t, 64> rgbaOut) noexcept;

} // namespace gfx::image
