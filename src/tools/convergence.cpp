// how long a still view takes to settle, from its captures: SPEEDO_CAMERA_PATH of one pose repeated, every n-th frame
// saved (SPEEDO_CAPTURE_PATH, SPEEDO_CAPTURE_PATH_STRIDE: pose-<frame>.bmp), at a fixed exposure (auto exposure settles
// by time, and its changes would be measured with the lighting's). see docs/performance-plan.md, step 1.
//
// usage: convergence <dir> [--settled <captures>] [--blur <pixels>] [--frame-ms <ms>]
//
// the settled image is the mean of the last --settled captures (default 8: their noise averaged). each capture's
// difference from it is the mean |log2| of their linear luminance (srgb decoded), both blurred over --blur pixels
// (default 24: the lighting's error rather than its grain), where the settled image is above a floor (black is noise in
// log terms). the noise is the last captures' mean difference from the settled image, at the blur and at 4 pixels: a
// difference that stays near it is noise, not unsettled light. prints the curve, the noise, and the frames (and seconds,
// given --frame-ms, the frame time measured without captures) after which the difference stays under 0.1, 0.05 and
// twice the noise.

#include <core/file.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

struct Luminance
{
	int width = 0;
	int height = 0;
	std::vector<float> values; // linear, top row first
};

// a 24 or 32 bit bmp as the client writes it (either row order) to its linear luminance
std::optional<Luminance> Load(const std::filesystem::path& path)
{
	static const auto kSrgb = []
	{
		std::array<float, 256> table{};
		for (int i = 0; i < 256; i++)
		{
			double c = i / 255.0;
			table[i] = static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
		}
		return table;
	}();
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
	if ((bytesPerPixel != 3 && bytesPerPixel != 4) || offset + stride * height > bytes.size())
	{
		std::println(stderr, "{}: not a 24 or 32 bit bmp", path.string());
		return std::nullopt;
	}
	Luminance image{width, height, std::vector<float>(static_cast<size_t>(width) * height)};
	for (int y = 0; y < height; y++)
	{
		const uint8_t* row = bytes.data() + offset + stride * static_cast<size_t>(bottomUp ? height - 1 - y : y);
		for (int x = 0; x < width; x++)
		{
			const uint8_t* bgr = row + static_cast<size_t>(x) * bytesPerPixel;
			image.values[static_cast<size_t>(y) * width + x] = 0.0722F * kSrgb[bgr[0]] + 0.7152F * kSrgb[bgr[1]] + 0.2126F * kSrgb[bgr[2]];
		}
	}
	return image;
}

// a box blur over radius pixels (a summed area table)
std::vector<float> Blur(const Luminance& image, int radius)
{
	int w = image.width;
	int h = image.height;
	std::vector<double> table(static_cast<size_t>(w + 1) * (h + 1), 0.0);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			size_t at = static_cast<size_t>(y + 1) * (w + 1) + x + 1;
			table[at] = image.values[static_cast<size_t>(y) * w + x] + table[at - 1] + table[at - (w + 1)] - table[at - (w + 1) - 1];
		}
	std::vector<float> blurred(image.values.size());
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			int x0 = std::max(x - radius, 0);
			int y0 = std::max(y - radius, 0);
			int x1 = std::min(x + radius + 1, w);
			int y1 = std::min(y + radius + 1, h);
			double sum = table[static_cast<size_t>(y1) * (w + 1) + x1] - table[static_cast<size_t>(y0) * (w + 1) + x1] -
						 table[static_cast<size_t>(y1) * (w + 1) + x0] + table[static_cast<size_t>(y0) * (w + 1) + x0];
			blurred[static_cast<size_t>(y) * w + x] = static_cast<float>(sum / (static_cast<double>(x1 - x0) * (y1 - y0)));
		}
	return blurred;
}

constexpr float kFloor = 0.002F;

double Difference(std::span<const float> image, std::span<const float> settled)
{
	double sum = 0.0;
	size_t count = 0;
	for (size_t at = 0; at < settled.size(); at++)
		if (settled[at] > kFloor)
		{
			sum += std::abs(std::log2(std::max(image[at], kFloor) / settled[at]));
			count++;
		}
	return count != 0 ? sum / static_cast<double>(count) : 0.0;
}

// the first frame after which the curve stays under the threshold, if it does
std::optional<int> SettledFrom(std::span<const std::pair<int, double>> curve, double threshold)
{
	std::optional<int> from;
	for (auto it = curve.rbegin(); it != curve.rend() && it->second < threshold; ++it)
		from = it->first;
	return from;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::println(stderr, "usage: convergence <dir> [--settled <captures>] [--blur <pixels>] [--frame-ms <ms>]");
		return 1;
	}
	std::filesystem::path dir = argv[1];
	size_t settledCount = 8;
	int radius = 24;
	double frameMs = 0.0;
	for (int argIt = 2; argIt + 1 < argc; argIt += 2)
	{
		std::string_view name = argv[argIt];
		if (name == "--settled")
			settledCount = std::strtoul(argv[argIt + 1], nullptr, 10);
		else if (name == "--blur")
			radius = std::atoi(argv[argIt + 1]);
		else if (name == "--frame-ms")
			frameMs = std::strtod(argv[argIt + 1], nullptr);
	}

	// the captures by frame (pose-<frame>.bmp)
	std::vector<std::pair<int, std::filesystem::path>> captures;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(dir, error))
	{
		auto name = entry.path().filename().string();
		if (name.starts_with("pose-") && name.ends_with(".bmp"))
			captures.emplace_back(std::atoi(name.c_str() + 5), entry.path());
	}
	std::ranges::sort(captures);
	if (error || captures.size() <= settledCount)
	{
		std::println(stderr, "{}: {} captures, too few for {} settled ones", dir.string(), captures.size(), settledCount);
		return 1;
	}

	// the settled image, blurred over the radius and over 4 pixels
	std::span settledCaptures(captures.end() - static_cast<ptrdiff_t>(settledCount), captures.end());
	std::vector<Luminance> lasts;
	for (const auto& capture : settledCaptures)
	{
		auto image = Load(capture.second);
		if (!image)
			return 1;
		lasts.push_back(std::move(*image));
	}
	Luminance settled{lasts.front().width, lasts.front().height, std::vector<float>(lasts.front().values.size(), 0.0F)};
	for (const auto& image : lasts)
		for (size_t at = 0; at < settled.values.size(); at++)
			settled.values[at] += image.values[at] / static_cast<float>(settledCount);
	auto settledWide = Blur(settled, radius);
	auto settledFine = Blur(settled, 4);
	double noiseWide = 0.0;
	double noiseFine = 0.0;
	for (const auto& image : lasts)
	{
		noiseWide += Difference(Blur(image, radius), settledWide) / static_cast<double>(settledCount);
		noiseFine += Difference(Blur(image, 4), settledFine) / static_cast<double>(settledCount);
	}

	std::vector<std::pair<int, double>> curve;
	for (const auto& [frame, path] : std::span(captures.begin(), captures.end() - static_cast<ptrdiff_t>(settledCount)))
	{
		auto image = Load(path);
		if (!image)
			return 1;
		if (image->width != settled.width || image->height != settled.height)
		{
			std::println(stderr, "{}: another size than the settled captures'", path.string());
			return 1;
		}
		curve.emplace_back(frame, Difference(Blur(*image, radius), settledWide));
	}

	for (size_t at = 0; at < curve.size(); at++)
		if (at < 10 || at % 5 == 0 || at + 1 == curve.size())
			std::println("frame {:5}  {:.4f}", curve[at].first, curve[at].second);
	std::println("noise: {:.4f} at {} pixels, {:.4f} at 4 (the last {} captures' mean difference from their mean)", noiseWide, radius, noiseFine, settledCount);
	for (double threshold : {0.1, 0.05, 2.0 * noiseWide})
	{
		auto from = SettledFrom(curve, threshold);
		if (!from)
			std::println("under {:.3f}: not within the run", threshold);
		else if (frameMs > 0.0)
			std::println("under {:.3f}: from frame {} ({:.1f} s at {:.1f} ms a frame)", threshold, *from, *from * frameMs / 1000.0, frameMs);
		else
			std::println("under {:.3f}: from frame {}", threshold, *from);
	}
	return 0;
}
