#include "imageimport.h"

#include <core/profiling.h>

#include <algorithm>
#include <bit>
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

namespace gfx::image
{

namespace detail
{

constexpr uint32_t kBlockDim = 4;
constexpr uint32_t kRgba = 4;

[[nodiscard]] constexpr uint32_t BlockCount(uint32_t extent) noexcept { return (extent + kBlockDim - 1) / kBlockDim; }

// compresses rgba pixels to blocks of format, row by row. pixels outside the image (in the blocks along the right and
// bottom edges when the extent isn't a multiple of 4) repeat the last row and column.
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
	bool alpha = format == Format::kBC3;

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

				auto* out = dst + ((static_cast<size_t>(blockRowIt) * blockColCount) + blockColIt) * blockSize;
				stb_compress_dxt_block(reinterpret_cast<unsigned char*>(out), block.data(), alpha ? 1 : 0, STB_DXT_HIGHQUAL);
			}
		});

	return !stopped;
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

} // namespace detail

std::expected<Image, std::string> Import(
	const std::filesystem::path& path,
	const std::function<std::byte*(size_t size)>& allocate,
	std::atomic_uint8_t* progress,
	const std::function<bool()>& cancelled)
{
	using namespace detail;

	ZoneScopedN("image::Import");

	int width = 0;
	int height = 0;
	int channelCount = 0;
	std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
		stbi_load(path.string().c_str(), &width, &height, &channelCount, STBI_rgb_alpha), &stbi_image_free);
	if (!pixels)
		return std::unexpected(std::format("failed to decode {}: {}", path.string(), stbi_failure_reason()));

	Image image;
	image.channelCount = static_cast<uint32_t>(channelCount);

	auto pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
	bool alpha = false;
	if (channelCount == 2 || channelCount == 4)
		for (size_t pixelIt = 0; pixelIt < pixelCount && !alpha; pixelIt++)
			alpha = pixels.get()[(pixelIt * kRgba) + 3] != 255;
	image.format = alpha ? Format::kBC3 : Format::kBC1;

	auto levelCount = static_cast<uint32_t>(std::bit_width(static_cast<uint32_t>(std::max(width, height))));
	image.mipLevels.resize(levelCount);
	size_t totalPixels = 0;
	for (uint32_t levelIt = 0; levelIt < levelCount; levelIt++)
	{
		auto& level = image.mipLevels[levelIt];
		level.width = std::max(static_cast<uint32_t>(width) >> levelIt, 1U);
		level.height = std::max(static_cast<uint32_t>(height) >> levelIt, 1U);
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

	std::vector<uint8_t> previous;
	std::vector<uint8_t> current;
	const uint8_t* src = pixels.get();
	for (uint32_t levelIt = 0; levelIt < levelCount; levelIt++)
	{
		const auto& level = image.mipLevels[levelIt];

		if (levelIt > 0)
		{
			ZoneScopedN("image::Import::resize");

			const auto& previousLevel = image.mipLevels[levelIt - 1];
			current.resize(static_cast<size_t>(level.width) * level.height * kRgba);
			if (stbir_resize_uint8_linear(
					src,
					static_cast<int>(previousLevel.width),
					static_cast<int>(previousLevel.height),
					static_cast<int>(previousLevel.width * kRgba),
					current.data(),
					static_cast<int>(level.width),
					static_cast<int>(level.height),
					static_cast<int>(level.width * kRgba),
					STBIR_RGBA) == nullptr)
				return std::unexpected(std::format("failed to resize {} to {}x{}", path.string(), level.width, level.height));

			std::swap(previous, current);
			src = previous.data();

			if (levelIt == 1)
				pixels.reset();
		}

		if (!CompressLevel(src, level.width, level.height, image.format, dst + level.offset, cancelled))
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

	std::array<uint8_t, 8> alphas{};
	alphas[0] = static_cast<uint8_t>(block[0]);
	alphas[1] = static_cast<uint8_t>(block[1]);
	if (alphas[0] > alphas[1])
	{
		for (uint32_t i = 1; i < 7; i++)
			alphas[i + 1] = static_cast<uint8_t>((((7 - i) * alphas[0]) + (i * alphas[1]) + 3) / 7);
	}
	else
	{
		for (uint32_t i = 1; i < 5; i++)
			alphas[i + 1] = static_cast<uint8_t>((((5 - i) * alphas[0]) + (i * alphas[1]) + 2) / 5);
		alphas[6] = 0;
		alphas[7] = 255;
	}

	uint64_t alphaIndices = 0;
	std::memcpy(&alphaIndices, block.data() + 2, 6);
	for (uint32_t pixelIt = 0; pixelIt < 16; pixelIt++)
		rgbaOut[(pixelIt * 4) + 3] = alphas[(alphaIndices >> (3 * pixelIt)) & 7U];
}

} // namespace gfx::image
