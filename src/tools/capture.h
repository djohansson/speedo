#pragma once

// the client's captures (SPEEDO_CAPTURE, SPEEDO_CAPTURE_PATH: bmps, 32 bit, top down) for the tools that compare them
// (convergence, imagecompare): read and written through core's files, their linear luminance, a box blur, and the mean
// |log2| difference of two luminances

#include <core/file.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <vector>

namespace capture
{

struct Image
{
	int width = 0;
	int height = 0;
	std::vector<uint8_t> bgr; // 3 bytes a pixel, srgb, top row first
};

// a 24 or 32 bit bmp, either row order
[[nodiscard]] inline std::optional<Image> Load(const std::filesystem::path& path)
{
	auto file = core::file::Map(path);
	if (!file || file->size() < 54)
	{
		std::println(stderr, "{}: can't read", path.string());
		return std::nullopt;
	}
	auto bytes = std::span(reinterpret_cast<const uint8_t*>(file->data()), file->size());
	auto read32 = [&](size_t at)
	{
		uint32_t value = 0;
		std::memcpy(&value, bytes.data() + at, sizeof(value));
		return value;
	};
	uint32_t offset = read32(10);
	auto width = static_cast<int32_t>(read32(18));
	auto height = static_cast<int32_t>(read32(22));
	int bytesPerPixel = (bytes[28] | (bytes[29] << 8)) / 8;
	bool bottomUp = height > 0;
	height = std::abs(height);
	size_t stride = (static_cast<size_t>(width) * bytesPerPixel + 3) & ~size_t{3};
	if (bytes[0] != 'B' || bytes[1] != 'M' || width <= 0 || (bytesPerPixel != 3 && bytesPerPixel != 4) ||
		offset + stride * height > bytes.size())
	{
		std::println(stderr, "{}: not a 24 or 32 bit bmp", path.string());
		return std::nullopt;
	}
	Image image{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 3)};
	for (int y = 0; y < height; y++)
	{
		const uint8_t* row = bytes.data() + offset + stride * static_cast<size_t>(bottomUp ? height - 1 - y : y);
		for (int x = 0; x < width; x++)
			std::memcpy(&image.bgr[(static_cast<size_t>(y) * width + x) * 3], row + static_cast<size_t>(x) * bytesPerPixel, 3);
	}
	return image;
}

// as a 32 bit bmp, top down, as the client writes them
[[nodiscard]] inline bool Save(const std::filesystem::path& path, const Image& image)
{
	size_t pixelBytes = static_cast<size_t>(image.width) * image.height * 4;
	std::vector<std::byte> file(54 + pixelBytes);
	auto put = [&file](size_t offset, uint32_t value, size_t count)
	{
		for (size_t byteIt = 0; byteIt < count; byteIt++)
			file[offset + byteIt] = static_cast<std::byte>((value >> (8 * byteIt)) & 0xFFU);
	};
	file[0] = std::byte{'B'};
	file[1] = std::byte{'M'};
	put(2, static_cast<uint32_t>(file.size()), 4);
	put(10, 54, 4);
	put(14, 40, 4);
	put(18, static_cast<uint32_t>(image.width), 4);
	put(22, static_cast<uint32_t>(-image.height), 4);
	put(26, 1, 2);
	put(28, 32, 2);
	put(34, static_cast<uint32_t>(pixelBytes), 4);
	for (size_t pixelIt = 0; pixelIt < static_cast<size_t>(image.width) * image.height; pixelIt++)
	{
		for (size_t channelIt = 0; channelIt < 3; channelIt++)
			file[54 + (pixelIt * 4) + channelIt] = static_cast<std::byte>(image.bgr[(pixelIt * 3) + channelIt]);
		file[54 + (pixelIt * 4) + 3] = std::byte{0xFF};
	}
	if (!core::file::Write(path, file))
	{
		std::println(stderr, "{}: can't write", path.string());
		return false;
	}
	return true;
}

// the linear luminance (rec. 709 weights over the srgb decoded channels), top row first
[[nodiscard]] inline std::vector<float> Luminance(const Image& image)
{
	static const auto kSrgb = []
	{
		std::array<float, 256> table{};
		for (int level = 0; level < 256; level++)
		{
			double c = level / 255.0;
			table[level] = static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
		}
		return table;
	}();
	std::vector<float> values(static_cast<size_t>(image.width) * image.height);
	for (size_t pixelIt = 0; pixelIt < values.size(); pixelIt++)
	{
		const uint8_t* bgr = &image.bgr[pixelIt * 3];
		values[pixelIt] = (0.0722F * kSrgb[bgr[0]]) + (0.7152F * kSrgb[bgr[1]]) + (0.2126F * kSrgb[bgr[2]]);
	}
	return values;
}

// a box blur over radius pixels each way (a summed area table)
[[nodiscard]] inline std::vector<float> Blur(std::span<const float> values, int width, int height, int radius)
{
	std::vector<double> table(static_cast<size_t>(width + 1) * (height + 1), 0.0);
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++)
		{
			size_t at = (static_cast<size_t>(y + 1) * (width + 1)) + x + 1;
			table[at] = values[(static_cast<size_t>(y) * width) + x] + table[at - 1] + table[at - (width + 1)] - table[at - (width + 1) - 1];
		}
	std::vector<float> blurred(values.size());
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++)
		{
			int x0 = std::max(x - radius, 0);
			int y0 = std::max(y - radius, 0);
			int x1 = std::min(x + radius + 1, width);
			int y1 = std::min(y + radius + 1, height);
			double sum = table[(static_cast<size_t>(y1) * (width + 1)) + x1] - table[(static_cast<size_t>(y0) * (width + 1)) + x1] -
						 table[(static_cast<size_t>(y1) * (width + 1)) + x0] + table[(static_cast<size_t>(y0) * (width + 1)) + x0];
			blurred[(static_cast<size_t>(y) * width) + x] = static_cast<float>(sum / (static_cast<double>(x1 - x0) * (y1 - y0)));
		}
	return blurred;
}

// below this linear luminance a pixel is left out of LogDifference: black is noise in log terms
constexpr float kLogFloor = 0.002F;

// the mean |log2| of image over reference, where the reference is above kLogFloor (the image clamped to it): a stop is 1
[[nodiscard]] inline double LogDifference(std::span<const float> image, std::span<const float> reference)
{
	double sum = 0.0;
	size_t count = 0;
	for (size_t at = 0; at < reference.size(); at++)
		if (reference[at] > kLogFloor)
		{
			sum += std::abs(std::log2(std::max(image[at], kLogFloor) / reference[at]));
			count++;
		}
	return count != 0 ? sum / static_cast<double>(count) : 0.0;
}

} // namespace capture
