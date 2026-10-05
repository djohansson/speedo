#include "imageimport.h"

#include <core/profiling.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <execution>
#include <format>
#include <memory>
#include <numeric>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

//NOLINTBEGIN(readability-magic-numbers)

namespace gfx::image
{

namespace detail
{

constexpr uint32_t kBlockDim = 4;
constexpr uint32_t kRgba = 4;

[[nodiscard]] constexpr uint32_t BlockCount(uint32_t extent) noexcept { return (extent + kBlockDim - 1) / kBlockDim; }

[[nodiscard]] Format FormatOf(Usage usage, bool alpha) noexcept
{
	switch (usage)
	{
	case Usage::kNormal:
	case Usage::kBump:
	case Usage::kMetallicRoughness: return Format::kBC5;
	case Usage::kMask:
	case Usage::kOcclusion: return Format::kBC4;
	default: return alpha ? Format::kBC3 : Format::kBC1;
	}
}

// compresses rgba pixels to blocks of format, row by row (bc4 from r, bc5 from r and g). pixels outside the image (in
// the blocks along the right and bottom edges when the extent isn't a multiple of 4) repeat the last row and column.
[[nodiscard]] bool CompressLevel(
	const uint8_t* src,
	uint32_t width,
	uint32_t height,
	Format format,
	std::byte* dst,
	const std::function<bool()>& cancelled)
{
	ZoneScopedN("image::CompressLevel");

	auto blockColCount = BlockCount(width);
	auto blockRowCount = BlockCount(height);
	auto blockSize = BlockSize(format);

	std::vector<uint32_t> blockRows(blockRowCount);
	std::ranges::iota(blockRows, 0U);

	std::atomic_bool stopped = false;
	std::for_each(
		std::execution::par,
		blockRows.begin(),
		blockRows.end(),
		[&](uint32_t blockRowIt)
		{
			if (stopped.load(std::memory_order_relaxed))
				return;

			if (cancelled && cancelled())
			{
				stopped = true;
				return;
			}

			std::array<uint8_t, kBlockDim * kBlockDim * kRgba> block;
			std::array<uint8_t, kBlockDim * kBlockDim * 2> channels;
			for (uint32_t blockColIt = 0; blockColIt < blockColCount; blockColIt++)
			{
				for (uint32_t y = 0; y < kBlockDim; y++)
				{
					auto row = std::min((blockRowIt * kBlockDim) + y, height - 1);
					for (uint32_t x = 0; x < kBlockDim; x++)
					{
						auto col = std::min((blockColIt * kBlockDim) + x, width - 1);
						std::copy_n(&src[((static_cast<size_t>(row) * width) + col) * kRgba], kRgba, &block[((y * kBlockDim) + x) * kRgba]);
					}
				}

				auto* out = reinterpret_cast<unsigned char*>(
					dst + ((static_cast<size_t>(blockRowIt) * blockColCount) + blockColIt) * blockSize);
				switch (format)
				{
				case Format::kBC1:
				case Format::kBC3:
					stb_compress_dxt_block(out, block.data(), format == Format::kBC3 ? 1 : 0, STB_DXT_HIGHQUAL);
					break;
				case Format::kBC4:
					for (uint32_t pixelIt = 0; pixelIt < kBlockDim * kBlockDim; pixelIt++)
						channels[pixelIt] = block[pixelIt * kRgba];
					stb_compress_bc4_block(out, channels.data());
					break;
				case Format::kBC5:
					for (uint32_t pixelIt = 0; pixelIt < kBlockDim * kBlockDim; pixelIt++)
					{
						channels[pixelIt * 2] = block[pixelIt * kRgba];
						channels[(pixelIt * 2) + 1] = block[(pixelIt * kRgba) + 1];
					}
					stb_compress_bc5_block(out, channels.data());
					break;
				}
			}
		});

	return !stopped;
}

[[nodiscard]] uint8_t Encode(float value) noexcept // [-1, 1] to [0, 255]
{
	return static_cast<uint8_t>(std::lround(std::clamp((value * 0.5F) + 0.5F, 0.0F, 1.0F) * 255.0F));
}

[[nodiscard]] float DecodeSigned(uint8_t value) noexcept // [0, 255] to [-1, 1]
{
	return (static_cast<float>(value) / 255.0F * 2.0F) - 1.0F;
}

void EncodeNormal(float x, float y, float z, uint8_t* rgba) noexcept
{
	auto length = std::sqrt((x * x) + (y * y) + (z * z));
	if (!(length > 0.0F))
	{
		x = y = 0.0F;
		z = length = 1.0F;
	}
	rgba[0] = Encode(x / length);
	rgba[1] = Encode(y / length);
	rgba[2] = Encode(z / length);
	rgba[3] = 255;
}

// normal maps are bluish (z mostly up) with x and y around 0 (0.5 encoded). a height map is grey, so it can't be both.
[[nodiscard]] bool IsNormalMap(const uint8_t* rgba, size_t pixelCount, uint32_t channelCount) noexcept
{
	if (channelCount < 3 || pixelCount == 0)
		return false;

	std::array<double, 3> mean{};
	for (size_t pixelIt = 0; pixelIt < pixelCount; pixelIt++)
		for (size_t ch = 0; ch < 3; ch++)
			mean[ch] += rgba[(pixelIt * kRgba) + ch];
	for (auto& m : mean)
		m /= 255.0 * static_cast<double>(pixelCount);

	return mean[2] > 0.6 && std::abs(mean[0] - 0.5) < 0.15 && std::abs(mean[1] - 0.5) < 0.15;
}

// scales every pixel's normal back to unit length
void RenormalizeLevel(std::vector<uint8_t>& rgba) noexcept
{
	for (size_t i = 0; i + 3 < rgba.size(); i += kRgba)
		EncodeNormal(DecodeSigned(rgba[i]), DecodeSigned(rgba[i + 1]), DecodeSigned(rgba[i + 2]), &rgba[i]);
}

// the 4 colors of a bc1 color block, as rgba. three colors and transparent black if fourColors is false.
void DecodeColors(std::span<const std::byte, 4> endpoints, bool fourColors, std::array<std::array<uint8_t, 4>, 4>& colorsOut)
{
	std::array<uint16_t, 2> c{};
	std::memcpy(c.data(), endpoints.data(), sizeof(c)); // little endian, as is everything we run on
	for (size_t i = 0; i < 2; i++)
	{
		auto r = (c[i] >> 11) & 31U;
		auto g = (c[i] >> 5) & 63U;
		auto b = c[i] & 31U;
		colorsOut[i] = {
			static_cast<uint8_t>((r << 3) | (r >> 2)),
			static_cast<uint8_t>((g << 2) | (g >> 4)),
			static_cast<uint8_t>((b << 3) | (b >> 2)),
			255};
	}

	for (size_t ch = 0; ch < 3; ch++)
	{
		if (fourColors)
		{
			colorsOut[2][ch] = static_cast<uint8_t>(((2 * colorsOut[0][ch]) + colorsOut[1][ch] + 1) / 3);
			colorsOut[3][ch] = static_cast<uint8_t>((colorsOut[0][ch] + (2 * colorsOut[1][ch]) + 1) / 3);
		}
		else
		{
			colorsOut[2][ch] = static_cast<uint8_t>((colorsOut[0][ch] + colorsOut[1][ch]) / 2);
			colorsOut[3][ch] = 0;
		}
	}
	colorsOut[2][3] = 255;
	colorsOut[3][3] = fourColors ? 255 : 0;
}

// the 16 values of an 8 byte bc3 alpha / bc4 / bc5 channel block
[[nodiscard]] std::array<uint8_t, 16> DecodeChannel(std::span<const std::byte> block) noexcept
{
	std::array<uint8_t, 8> values{};
	values[0] = static_cast<uint8_t>(block[0]);
	values[1] = static_cast<uint8_t>(block[1]);
	if (values[0] > values[1])
	{
		for (uint32_t i = 1; i < 7; i++)
			values[i + 1] = static_cast<uint8_t>((((7 - i) * values[0]) + (i * values[1]) + 3) / 7);
	}
	else
	{
		for (uint32_t i = 1; i < 5; i++)
			values[i + 1] = static_cast<uint8_t>((((5 - i) * values[0]) + (i * values[1]) + 2) / 5);
		values[6] = 0;
		values[7] = 255;
	}

	uint64_t indices = 0;
	std::memcpy(&indices, block.data() + 2, 6);

	std::array<uint8_t, 16> out{};
	for (uint32_t pixelIt = 0; pixelIt < 16; pixelIt++)
		out[pixelIt] = values[(indices >> (3 * pixelIt)) & 7U];
	return out;
}

} // namespace detail

std::expected<Pixels, std::string> Decode(const std::filesystem::path& path, const Options& options)
{
	using namespace detail;

	ZoneScopedN("image::Decode");

	int width = 0;
	int height = 0;
	int channelCount = 0;
	std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
		stbi_load(path.string().c_str(), &width, &height, &channelCount, STBI_rgb_alpha), &stbi_image_free);
	if (!decoded)
		return std::unexpected(std::format("failed to decode {}: {}", path.string(), stbi_failure_reason()));

	Pixels pixels{
		.width = static_cast<uint32_t>(width),
		.height = static_cast<uint32_t>(height),
		.channelCount = static_cast<uint32_t>(channelCount)};
	auto pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
	pixels.rgba.assign(decoded.get(), decoded.get() + (pixelCount * kRgba));
	decoded.reset();

	auto& rgba = pixels.rgba;

	bool alpha = false;
	if (channelCount == 2 || channelCount == 4)
		for (size_t pixelIt = 0; pixelIt < pixelCount && !alpha; pixelIt++)
			alpha = rgba[(pixelIt * kRgba) + 3] != 255;

	switch (options.usage)
	{
	case Usage::kColor:
	case Usage::kLinear:
		pixels.alpha = alpha;
		break;

	case Usage::kMask:
		for (size_t i = 0; i < rgba.size(); i += kRgba)
		{
			auto value = alpha ? rgba[i + 3]
							   : static_cast<uint8_t>(std::lround((0.299 * rgba[i]) + (0.587 * rgba[i + 1]) + (0.114 * rgba[i + 2])));
			rgba[i] = rgba[i + 1] = rgba[i + 2] = value;
			rgba[i + 3] = 255;
		}
		break;

	case Usage::kOcclusion:
		for (size_t i = 0; i < rgba.size(); i += kRgba)
		{
			rgba[i + 1] = rgba[i + 2] = rgba[i];
			rgba[i + 3] = 255;
		}
		break;

	case Usage::kMetallicRoughness:
		for (size_t i = 0; i < rgba.size(); i += kRgba)
		{
			rgba[i] = rgba[i + 1];
			rgba[i + 1] = rgba[i + 2];
			rgba[i + 2] = 0;
			rgba[i + 3] = 255;
		}
		break;

	case Usage::kNormal:
	case Usage::kBump:
		if (options.usage == Usage::kNormal || IsNormalMap(rgba.data(), pixelCount, pixels.channelCount))
		{
			// +y up to +y down the image
			for (size_t i = 0; i < rgba.size(); i += kRgba)
				EncodeNormal(DecodeSigned(rgba[i]), -DecodeSigned(rgba[i + 1]), DecodeSigned(rgba[i + 2]), &rgba[i]);
		}
		else
		{
			pixels.fromHeight = true;

			// the luminance as height, and its slope (central differences, wrapping around since textures tile) as the
			// normal: n = (-dh/du, -dh/dv, 1), with u and v in widths of the image. bumpScale is the full range's depth
			// in 1/64ths of the width.
			std::vector<float> heights(pixelCount);
			for (size_t pixelIt = 0; pixelIt < pixelCount; pixelIt++)
			{
				const auto* p = &rgba[pixelIt * kRgba];
				heights[pixelIt] = static_cast<float>((0.299 * p[0]) + (0.587 * p[1]) + (0.114 * p[2])) / 255.0F;
			}

			auto scale = options.bumpScale * static_cast<float>(width) / 64.0F;
			auto at = [&heights, width, height](int x, int y)
			{
				return heights[(static_cast<size_t>((y + height) % height) * width) + ((x + width) % width)];
			};
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					auto dx = (at(x + 1, y) - at(x - 1, y)) * 0.5F;
					auto dy = (at(x, y + 1) - at(x, y - 1)) * 0.5F;
					EncodeNormal(-dx * scale, -dy * scale, 1.0F, &rgba[((static_cast<size_t>(y) * width) + x) * kRgba]);
				}
			}
		}
		break;
	}

	return pixels;
}

std::expected<Image, std::string> Import(
	const std::filesystem::path& path,
	const Options& options,
	const std::function<std::byte*(size_t size)>& allocate,
	std::atomic_uint8_t* progress,
	const std::function<bool()>& cancelled)
{
	using namespace detail;

	ZoneScopedN("image::Import");

	auto pixels = Decode(path, options);
	if (!pixels)
		return std::unexpected(pixels.error());

	Image image{
		.channelCount = pixels->channelCount,
		.format = FormatOf(options.usage, pixels->alpha),
		.usage = options.usage,
		.fromHeight = pixels->fromHeight};

	auto width = pixels->width;
	auto height = pixels->height;
	auto levelCount = static_cast<uint32_t>(std::bit_width(std::max(width, height)));
	image.mipLevels.resize(levelCount);
	size_t totalPixels = 0;
	for (uint32_t levelIt = 0; levelIt < levelCount; levelIt++)
	{
		auto& level = image.mipLevels[levelIt];
		level.width = std::max(width >> levelIt, 1U);
		level.height = std::max(height >> levelIt, 1U);
		level.offset = static_cast<uint32_t>(image.size);
		level.size = BlockCount(level.width) * BlockCount(level.height) * BlockSize(image.format);
		image.size += level.size;
		totalPixels += static_cast<size_t>(level.width) * level.height;
	}

	if (image.size > std::numeric_limits<uint32_t>::max())
		return std::unexpected(std::format("{} is too large: {}x{}", path.string(), width, height));

	auto* dst = allocate(image.size);
	if (dst == nullptr)
		return std::unexpected(std::format("failed to allocate {} bytes for {}", image.size, path.string()));

	constexpr uint8_t kProgressEnd = 224;
	auto progressBegin = progress != nullptr ? std::min(progress->load(), kProgressEnd) : kProgressEnd;
	size_t pixelsDone = 0;

	std::vector<uint8_t> previous = std::move(pixels->rgba);
	std::vector<uint8_t> current;
	for (uint32_t levelIt = 0; levelIt < levelCount; levelIt++)
	{
		const auto& level = image.mipLevels[levelIt];

		if (levelIt > 0)
		{
			ZoneScopedN("image::Import::resize");

			const auto& previousLevel = image.mipLevels[levelIt - 1];
			current.resize(static_cast<size_t>(level.width) * level.height * kRgba);
			auto resize = options.usage == Usage::kColor ? &stbir_resize_uint8_srgb : &stbir_resize_uint8_linear;
			if (resize(
					previous.data(),
					static_cast<int>(previousLevel.width),
					static_cast<int>(previousLevel.height),
					static_cast<int>(previousLevel.width * kRgba),
					current.data(),
					static_cast<int>(level.width),
					static_cast<int>(level.height),
					static_cast<int>(level.width * kRgba),
					STBIR_RGBA) == nullptr)
				return std::unexpected(std::format("failed to resize {} to {}x{}", path.string(), level.width, level.height));

			// averaged normals are shorter than unit length
			if (options.usage == Usage::kNormal || options.usage == Usage::kBump)
				RenormalizeLevel(current);

			std::swap(previous, current);
		}

		if (!CompressLevel(previous.data(), level.width, level.height, image.format, dst + level.offset, cancelled))
			return std::unexpected("cancelled");

		pixelsDone += static_cast<size_t>(level.width) * level.height;
		if (progress != nullptr)
			*progress = static_cast<uint8_t>(progressBegin + ((kProgressEnd - progressBegin) * pixelsDone / totalPixels));
	}

	return image;
}

void DecompressBlock(Format format, std::span<const std::byte> block, std::span<uint8_t, 64> rgbaOut) noexcept
{
	using namespace detail;

	if (format == Format::kBC4 || format == Format::kBC5)
	{
		auto x = DecodeChannel(block.subspan(0, 8));
		auto y = format == Format::kBC5 ? DecodeChannel(block.subspan(8, 8)) : x;
		for (uint32_t pixelIt = 0; pixelIt < 16; pixelIt++)
		{
			auto* out = &rgbaOut[pixelIt * 4];
			if (format == Format::kBC4)
			{
				out[0] = out[1] = out[2] = x[pixelIt];
			}
			else
			{
				auto nx = DecodeSigned(x[pixelIt]);
				auto ny = DecodeSigned(y[pixelIt]);
				out[0] = x[pixelIt];
				out[1] = y[pixelIt];
				out[2] = Encode(std::sqrt(std::max(0.0F, 1.0F - (nx * nx) - (ny * ny))));
			}
			out[3] = 255;
		}
		return;
	}

	auto colorBlock = format == Format::kBC3 ? block.subspan(8, 8) : block.subspan(0, 8);

	std::array<uint16_t, 2> endpoints{};
	std::memcpy(endpoints.data(), colorBlock.data(), sizeof(endpoints));
	// bc3's color block always has four colors
	bool fourColors = format == Format::kBC3 || endpoints[0] > endpoints[1];

	std::array<std::array<uint8_t, 4>, 4> colors{};
	DecodeColors(colorBlock.first<4>(), fourColors, colors);

	uint32_t indices = 0;
	std::memcpy(&indices, colorBlock.data() + 4, sizeof(indices));
	for (uint32_t pixelIt = 0; pixelIt < 16; pixelIt++)
		std::ranges::copy(colors[(indices >> (2 * pixelIt)) & 3U], rgbaOut.begin() + (pixelIt * 4));

	if (format != Format::kBC3)
		return;

	auto alphas = DecodeChannel(block.subspan(0, 8));
	for (uint32_t pixelIt = 0; pixelIt < 16; pixelIt++)
		rgbaOut[(pixelIt * 4) + 3] = alphas[pixelIt];
}

} // namespace gfx::image

//NOLINTEND(readability-magic-numbers)
