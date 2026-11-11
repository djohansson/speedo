// compares a capture of the client's with a reference (bmps, see capture.h): what a visual check passes or fails by (see
// docs/visual-testing-plan.md, step 2).
//
// usage: imagecompare <reference.bmp> <capture.bmp> [--diff <out.bmp>] [--blur <pixels>] [thresholds]
//        imagecompare --self-test
//
// measures the capture against the reference: per channel the largest and the mean difference, in levels of 255; the
// share of the pixels whose largest channel difference passes 0, 1, 4 and 16 levels, and where those are (their bounding
// box: a region gone wrong, which a mean hides); and the mean |log2| of the two linear luminances (a stop is 1), sharp and
// blurred over --blur pixels each way (default 8: the low frequencies, the lighting's error rather than its noise).
// thresholds, each failing the comparison when passed (exit 1; 0 within all of them, 2 on an error):
//   --max <levels>             the largest channel difference
//   --mean <levels>            the mean difference of the channel whose mean is largest
//   --over <levels>:<percent>  the share of the pixels past <levels> (more than one may be given)
//   --log <stops>              the sharp mean |log2| luminance difference
//   --log-blurred <stops>      the blurred one
// --diff writes the differences as a bmp, amplified 16 times (clamped), black where the two are the same.
// --self-test checks the measures on made-up pairs (the same image, a pixel off, an edge moved, noise, twice the light).

#include "capture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <numeric>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr std::array<int, 4> kOverLevels{0, 1, 4, 16};
constexpr int kDiffScale = 16;

struct Measures
{
	std::array<int, 3> max{}; // b, g, r
	std::array<double, 3> mean{};
	std::array<double, kOverLevels.size()> overShare{}; // of the pixels, past kOverLevels' levels
	std::optional<std::array<int, 4>> differing; // x0, y0, x1, y1 (inclusive) of the pixels that differ at all
	double log = 0.0;
	double logBlurred = 0.0;
	double ratio = 0.0; // log2 of the capture's mean linear luminance over the reference's: light gained or lost, in stops
	std::vector<int> largest; // each pixel's largest channel difference, for --over and --diff
};

[[nodiscard]] Measures Compare(const capture::Image& reference, const capture::Image& image, int blur)
{
	Measures measures;
	size_t pixelCount = static_cast<size_t>(reference.width) * reference.height;
	measures.largest.resize(pixelCount);
	std::array<double, 3> sums{};
	std::array<size_t, kOverLevels.size()> over{};
	for (size_t pixelIt = 0; pixelIt < pixelCount; pixelIt++)
	{
		int largest = 0;
		for (size_t channelIt = 0; channelIt < 3; channelIt++)
		{
			int difference = std::abs(int{reference.bgr[(pixelIt * 3) + channelIt]} - int{image.bgr[(pixelIt * 3) + channelIt]});
			measures.max[channelIt] = std::max(measures.max[channelIt], difference);
			sums[channelIt] += difference;
			largest = std::max(largest, difference);
		}
		measures.largest[pixelIt] = largest;
		for (size_t levelIt = 0; levelIt < kOverLevels.size(); levelIt++)
			if (largest > kOverLevels[levelIt])
				over[levelIt]++;
		if (largest > 0)
		{
			int x = static_cast<int>(pixelIt % static_cast<size_t>(reference.width));
			int y = static_cast<int>(pixelIt / static_cast<size_t>(reference.width));
			auto& box = measures.differing;
			box = box ? std::array<int, 4>{std::min((*box)[0], x), std::min((*box)[1], y), std::max((*box)[2], x), std::max((*box)[3], y)}
					  : std::array<int, 4>{x, y, x, y};
		}
	}
	for (size_t channelIt = 0; channelIt < 3; channelIt++)
		measures.mean[channelIt] = sums[channelIt] / static_cast<double>(std::max<size_t>(pixelCount, 1));
	for (size_t levelIt = 0; levelIt < kOverLevels.size(); levelIt++)
		measures.overShare[levelIt] = static_cast<double>(over[levelIt]) / static_cast<double>(std::max<size_t>(pixelCount, 1));

	auto referenceLuminance = capture::Luminance(reference);
	auto imageLuminance = capture::Luminance(image);
	measures.log = capture::LogDifference(imageLuminance, referenceLuminance);
	measures.logBlurred = capture::LogDifference(
		capture::Blur(imageLuminance, image.width, image.height, blur), capture::Blur(referenceLuminance, reference.width, reference.height, blur));
	auto mean = [](const std::vector<float>& values)
	{ return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(std::max<size_t>(values.size(), 1)); };
	// both black: the same light. one black: as far apart as a ratio can say
	double referenceMean = std::max(mean(referenceLuminance), static_cast<double>(capture::kLogFloor));
	double imageMean = std::max(mean(imageLuminance), static_cast<double>(capture::kLogFloor));
	measures.ratio = std::log2(imageMean / referenceMean);
	return measures;
}

[[nodiscard]] double ShareOver(const Measures& measures, int levels)
{
	size_t count = std::ranges::count_if(measures.largest, [levels](int largest) { return largest > levels; });
	return static_cast<double>(count) / static_cast<double>(std::max<size_t>(measures.largest.size(), 1));
}

void Print(const Measures& measures, int blur)
{
	std::println("max |difference| r {} g {} b {}, mean r {:.4f} g {:.4f} b {:.4f} (of 255)", measures.max[2], measures.max[1], measures.max[0],
				 measures.mean[2], measures.mean[1], measures.mean[0]);
	for (size_t levelIt = 0; levelIt < kOverLevels.size(); levelIt++)
		std::println("  pixels past {:2} levels: {:.4f}%", kOverLevels[levelIt], 100.0 * measures.overShare[levelIt]);
	if (measures.differing)
		std::println("  differing within x {}..{}, y {}..{}", (*measures.differing)[0], (*measures.differing)[2], (*measures.differing)[1], (*measures.differing)[3]);
	std::println("mean |log2| luminance: {:.5f} stops, {:.5f} blurred over {} pixels", measures.log, measures.logBlurred, blur);
	std::println("mean luminance ratio: {:+.5f} stops (the capture's light against the reference's)", measures.ratio);
}

[[nodiscard]] capture::Image Differences(const Measures& measures, const capture::Image& reference, const capture::Image& image)
{
	capture::Image diff{reference.width, reference.height, std::vector<uint8_t>(reference.bgr.size())};
	for (size_t at = 0; at < diff.bgr.size(); at++)
		diff.bgr[at] = static_cast<uint8_t>(std::min(std::abs(int{reference.bgr[at]} - int{image.bgr[at]}) * kDiffScale, 255));
	return diff;
}

struct Thresholds
{
	std::optional<int> max;
	std::optional<double> mean;
	std::vector<std::pair<int, double>> over; // levels, share (0 to 1)
	std::optional<double> log;
	std::optional<double> logBlurred;
	std::optional<double> ratio; // either way, of expectRatio
	double expectRatio = 0.0; // the ratio expected (a known answer's: a furnace's light over its emission), 0 by default
};

// whether the measures are within the thresholds, saying which one each is past (unless quiet)
[[nodiscard]] bool Within(const Measures& measures, const Thresholds& thresholds, bool quiet = false)
{
	bool within = true;
	auto fail = [&within, quiet]<typename... Args>(std::format_string<Args...> format, Args&&... args)
	{
		if (!quiet)
			std::println("FAIL: {}", std::format(format, std::forward<Args>(args)...));
		within = false;
	};
	int max = std::ranges::max(measures.max);
	double mean = std::ranges::max(measures.mean);
	if (thresholds.max && max > *thresholds.max)
		fail("max {} levels, past {}", max, *thresholds.max);
	if (thresholds.mean && mean > *thresholds.mean)
		fail("mean {:.4f} levels, past {:.4f}", mean, *thresholds.mean);
	for (auto [levels, share] : thresholds.over)
		if (double measured = ShareOver(measures, levels); measured > share)
			fail("{:.4f}% of the pixels past {} levels, more than {:.4f}%", 100.0 * measured, levels, 100.0 * share);
	if (thresholds.log && measures.log > *thresholds.log)
		fail("mean |log2| luminance {:.5f} stops, past {:.5f}", measures.log, *thresholds.log);
	if (thresholds.logBlurred && measures.logBlurred > *thresholds.logBlurred)
		fail("blurred mean |log2| luminance {:.5f} stops, past {:.5f}", measures.logBlurred, *thresholds.logBlurred);
	if (thresholds.ratio && std::abs(measures.ratio - thresholds.expectRatio) > *thresholds.ratio)
		fail("mean luminance ratio {:+.5f} stops, past {:.5f} either way of {:+.5f}", measures.ratio, *thresholds.ratio, thresholds.expectRatio);
	return within;
}

// made-up pairs whose measures are known
[[nodiscard]] int SelfTest()
{
	constexpr int kWidth = 64;
	constexpr int kHeight = 32;
	auto make = [](auto&& pixel)
	{
		capture::Image image{kWidth, kHeight, std::vector<uint8_t>(static_cast<size_t>(kWidth) * kHeight * 3)};
		for (int y = 0; y < kHeight; y++)
			for (int x = 0; x < kWidth; x++)
				for (int channelIt = 0; channelIt < 3; channelIt++)
					image.bgr[((static_cast<size_t>(y) * kWidth + x) * 3) + channelIt] = pixel(x, y, channelIt);
		return image;
	};
	auto gray = [](int /*x*/, int /*y*/, int /*channel*/) { return uint8_t{128}; };
	int failures = 0;
	auto expect = [&failures](bool condition, std::string_view what)
	{
		std::println("{}: {}", condition ? "ok" : "FAILED", what);
		failures += condition ? 0 : 1;
	};
	constexpr int kBlur = 8;
	{
		auto measures = Compare(make(gray), make(gray), kBlur);
		expect(std::ranges::max(measures.max) == 0 && !measures.differing && measures.log == 0.0, "the same image: no difference");
	}
	{
		auto measures = Compare(make(gray), make([](int x, int y, int channel) { return uint8_t(x == 10 && y == 5 && channel == 2 ? 178 : 128); }), kBlur);
		expect(measures.max[2] == 50 && measures.max[0] == 0 && ShareOver(measures, 16) == 1.0 / (kWidth * kHeight), "a pixel off by 50 in red");
		expect(measures.differing == std::array<int, 4>{10, 5, 10, 5}, "and where it is");
		expect(Within(measures, Thresholds{.max = 60}, true) && !Within(measures, Thresholds{.max = 40}, true), "--max 60 passes it, --max 40 fails it");
	}
	{
		auto edge = [](int at) { return [at](int x, int /*y*/, int /*channel*/) { return uint8_t(x < at ? 20 : 230); }; };
		auto measures = Compare(make(edge(32)), make(edge(33)), kBlur);
		expect(ShareOver(measures, 16) == 1.0 / kWidth && measures.differing == std::array<int, 4>{32, 0, 32, kHeight - 1}, "an edge moved a pixel: one column");
		expect(measures.logBlurred < measures.log, "which blurring mostly hides");
	}
	{
		auto noise = [](int x, int y, int /*channel*/) { return uint8_t(128 + (((x * 7) + (y * 13)) % 3) - 1); };
		auto measures = Compare(make(gray), make(noise), kBlur);
		expect(std::ranges::max(measures.max) == 1 && ShareOver(measures, 1) == 0.0 && ShareOver(measures, 0) > 0.5, "noise of a level: max 1, none past 1");
		expect(Within(measures, Thresholds{.max = 1, .over = {{0, 0.7}}}, true), "--max 1 --over 0:70 passes it");
	}
	{
		// twice the linear light: srgb 128 is 0.2159 linear, and twice that srgb 175.6 (176 is 2.011 times as bright)
		auto measures = Compare(make(gray), make([](int, int, int) { return uint8_t{176}; }), kBlur);
		expect(std::abs(measures.log - 1.0) < 0.01 && std::abs(measures.logBlurred - 1.0) < 0.01, "twice the light: a stop, sharp and blurred");
		expect(std::abs(measures.ratio - 1.0) < 0.01, "and a mean luminance ratio of +1 stop");
		expect(Within(measures, Thresholds{.ratio = 1.1}, true) && !Within(measures, Thresholds{.ratio = 0.5}, true), "--ratio 1.1 passes it, --ratio 0.5 fails it");
		expect(Within(measures, Thresholds{.ratio = 0.05, .expectRatio = 1.0}, true) && !Within(measures, Thresholds{.ratio = 0.05, .expectRatio = 0.5}, true),
			   "--expect-ratio 1 --ratio 0.05 passes it, --expect-ratio 0.5 fails it");
	}
	{
		// the light moved, not lost: a bright half and a dark half swapped
		auto half = [](bool left) { return [left](int x, int /*y*/, int /*channel*/) { return uint8_t((x < kWidth / 2) == left ? 200 : 60); }; };
		auto measures = Compare(make(half(true)), make(half(false)), kBlur);
		expect(std::abs(measures.ratio) < 0.001 && measures.log > 1.0, "light moved, not lost: a ratio of 0, a large log difference");
	}
	std::println("{} of the self test's checks failed", failures);
	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 2 && std::string_view(argv[1]) == "--self-test")
		return SelfTest();
	if (argc < 3)
	{
		std::println(stderr, "usage: imagecompare <reference.bmp> <capture.bmp> [--diff <out.bmp>] [--blur <pixels>] [--max <levels>] [--mean <levels>]");
		std::println(stderr, "                    [--over <levels>:<percent>]... [--log <stops>] [--log-blurred <stops>]");
		std::println(stderr, "       imagecompare --self-test");
		return 2;
	}
	std::filesystem::path referencePath = argv[1];
	std::filesystem::path imagePath = argv[2];
	std::optional<std::filesystem::path> diffPath;
	int blur = 8;
	Thresholds thresholds;
	for (int argIt = 3; argIt < argc; argIt++)
	{
		std::string_view name = argv[argIt];
		if (argIt + 1 >= argc)
		{
			std::println(stderr, "{}: no value", name);
			return 2;
		}
		const char* value = argv[++argIt];
		if (name == "--diff")
			diffPath = value;
		else if (name == "--blur")
			blur = std::max(std::atoi(value), 0);
		else if (name == "--max")
			thresholds.max = std::atoi(value);
		else if (name == "--mean")
			thresholds.mean = std::strtod(value, nullptr);
		else if (name == "--log")
			thresholds.log = std::strtod(value, nullptr);
		else if (name == "--log-blurred")
			thresholds.logBlurred = std::strtod(value, nullptr);
		else if (name == "--ratio")
			thresholds.ratio = std::strtod(value, nullptr);
		else if (name == "--expect-ratio")
			thresholds.expectRatio = std::strtod(value, nullptr);
		else if (name == "--over")
		{
			std::string_view text = value;
			auto colon = text.find(':');
			if (colon == std::string_view::npos)
			{
				std::println(stderr, "--over {}: expected <levels>:<percent>", text);
				return 2;
			}
			thresholds.over.emplace_back(std::atoi(std::string(text.substr(0, colon)).c_str()), std::strtod(std::string(text.substr(colon + 1)).c_str(), nullptr) / 100.0);
		}
		else
		{
			std::println(stderr, "unknown option {}", name);
			return 2;
		}
	}

	auto reference = capture::Load(referencePath);
	auto image = capture::Load(imagePath);
	if (!reference || !image)
		return 2;
	if (reference->width != image->width || reference->height != image->height)
	{
		std::println(stderr, "sizes differ: {}x{} and {}x{}", reference->width, reference->height, image->width, image->height);
		return 2;
	}
	std::println("{}x{}", reference->width, reference->height);
	auto measures = Compare(*reference, *image, blur);
	Print(measures, blur);
	if (diffPath && !capture::Save(*diffPath, Differences(measures, *reference, *image)))
		return 2;
	return Within(measures, thresholds) ? 0 : 1;
}
