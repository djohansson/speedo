// how long a still view takes to settle, from its captures: SPEEDO_CAMERA_PATH of one pose repeated, every n-th frame
// saved (SPEEDO_CAPTURE_PATH, SPEEDO_CAPTURE_PATH_STRIDE: pose-<frame>.bmp), at a fixed exposure (auto exposure settles
// by time, and its changes would be measured with the lighting's). see docs/performance-plan.md, step 1.
//
// usage: convergence <dir> [--settled <captures>] [--blur <pixels>] [--frame-ms <ms>] [--warm-ms <ms> --warm-frames <n>]
//
// the settled image is the mean of the last --settled captures (default 8: their noise averaged). each capture's
// difference from it is the mean |log2| of their linear luminance (srgb decoded), both blurred over --blur pixels
// (default 24: the lighting's error rather than its grain), where the settled image is above a floor (black is noise in
// log terms). the noise is the last captures' mean difference from the settled image, at the blur and at 4 pixels: a
// difference that stays near it is noise, not unsettled light. prints the curve, the noise, and the frames (and seconds,
// given --frame-ms, the frame time measured without captures; the first --warm-frames frames at --warm-ms, the lighting's
// warm-up, see Renderer::IsLightingWarmingUp) after which the difference stays under 0.1, 0.05 and twice the noise.

#include "capture.h"

#include <algorithm>
#include <cstdlib>
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

// a capture's linear luminance (see capture.h, which imagecompare shares)
std::optional<Luminance> Load(const std::filesystem::path& path)
{
	auto image = capture::Load(path);
	if (!image)
		return std::nullopt;
	return Luminance{.width = image->width, .height = image->height, .values = capture::Luminance(*image)};
}

std::vector<float> Blur(const Luminance& image, int radius) { return capture::Blur(image.values, image.width, image.height, radius); }

double Difference(std::span<const float> image, std::span<const float> settled) { return capture::LogDifference(image, settled); }

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
		std::println(stderr, "usage: convergence <dir> [--settled <captures>] [--blur <pixels>] [--frame-ms <ms>] [--warm-ms <ms> --warm-frames <n>]");
		return 1;
	}
	std::filesystem::path dir = argv[1];
	size_t settledCount = 8;
	int radius = 24;
	double frameMs = 0.0;
	double warmMs = 0.0;
	int warmFrameCount = 0;
	for (int argIt = 2; argIt + 1 < argc; argIt += 2)
	{
		std::string_view name = argv[argIt];
		if (name == "--settled")
			settledCount = std::strtoul(argv[argIt + 1], nullptr, 10);
		else if (name == "--blur")
			radius = std::atoi(argv[argIt + 1]);
		else if (name == "--frame-ms")
			frameMs = std::strtod(argv[argIt + 1], nullptr);
		else if (name == "--warm-ms")
			warmMs = std::strtod(argv[argIt + 1], nullptr);
		else if (name == "--warm-frames")
			warmFrameCount = std::atoi(argv[argIt + 1]);
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
		else if (frameMs > 0.0 && warmMs > 0.0 && warmFrameCount > 0)
		{
			int warm = std::min(*from, warmFrameCount);
			double seconds = (warm * warmMs + (*from - warm) * frameMs) / 1000.0;
			std::println(
				"under {:.3f}: from frame {} ({:.1f} s at {:.1f} ms a frame, the first {} at {:.1f})", threshold, *from, seconds, frameMs, warmFrameCount, warmMs);
		}
		else if (frameMs > 0.0)
			std::println("under {:.3f}: from frame {} ({:.1f} s at {:.1f} ms a frame)", threshold, *from, *from * frameMs / 1000.0, frameMs);
		else
			std::println("under {:.3f}: from frame {}", threshold, *from);
	}
	return 0;
}
