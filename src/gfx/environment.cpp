#include "environment.h"

#include <core/profiling.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <execution>
#include <format>
#include <memory>
#include <numbers>
#include <numeric>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

// the implementations are in imageimport.cpp
#include <stb_image.h>
#include <stb_image_resize2.h>

namespace gfx::environment
{

namespace detail
{

constexpr float kPi = std::numbers::pi_v<float>;

// the direction at the center of a panorama texel (see Panorama)
glm::vec3 Direction(float u, float v)
{
	float phi = (u - 0.5F) * 2.0F * kPi;
	float theta = v * kPi;
	return {std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi)};
}

// the sine of a panorama row's polar angle: its texels' solid angle relative to the equator's
float RowWeight(uint32_t y, uint32_t height)
{
	return std::sin((static_cast<float>(y) + 0.5F) / static_cast<float>(height) * kPi);
}

// the real spherical harmonics basis, bands 0 to 2
std::array<float, 9> ShBasis(const glm::vec3& d)
{
	return {
		0.282095F,
		0.488603F * d.y,
		0.488603F * d.z,
		0.488603F * d.x,
		1.092548F * d.x * d.y,
		1.092548F * d.y * d.z,
		0.315392F * ((3.0F * d.z * d.z) - 1.0F),
		1.092548F * d.x * d.z,
		0.546274F * ((d.x * d.x) - (d.y * d.y)),
	};
}

} // namespace detail

std::expected<Panorama, std::string> Import(const std::filesystem::path& path, uint32_t maxWidth)
{
	ZoneScopedN("gfx::environment::Import");

	int width = 0;
	int height = 0;
	int channelCount = 0;
	std::unique_ptr<float, decltype(&stbi_image_free)> pixels(
		stbi_loadf(path.string().c_str(), &width, &height, &channelCount, 3), &stbi_image_free);
	if (!pixels)
		return std::unexpected(std::format("{}: can't read the panorama: {}", path.string(), stbi_failure_reason()));
	if (width != 2 * height)
		return std::unexpected(std::format("{}: a panorama is twice as wide as high, not {}x{}", path.string(), width, height));

	Panorama panorama{.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)};
	if (panorama.width > maxWidth)
	{
		panorama.width = maxWidth;
		panorama.height = maxWidth / 2;
		panorama.rgb.resize(static_cast<size_t>(panorama.width) * panorama.height * 3);
		if (stbir_resize_float_linear(
				pixels.get(), width, height, 0, panorama.rgb.data(), static_cast<int>(panorama.width), static_cast<int>(panorama.height), 0,
				STBIR_RGB) == nullptr)
			return std::unexpected(std::format("{}: can't scale the panorama", path.string()));
	}
	else
	{
		panorama.rgb.assign(pixels.get(), pixels.get() + (static_cast<size_t>(width) * height * 3));
	}

	// infinities (some files have them for the sun) and negatives would spread through the filtering
	for (auto& value : panorama.rgb)
		value = std::isfinite(value) ? std::clamp(value, 0.0F, 65504.0F) : 0.0F;

	return panorama;
}

const SkyParameters& ProceduralSkyParameters()
{
	static const SkyParameters kSky = []
	{
		SkyParameters sky{
			.zenith = {0.25F, 0.40F, 0.75F},
			.horizon = {0.80F, 0.82F, 0.85F},
			.ground = {0.30F, 0.27F, 0.24F},
			.sunCosRadius = std::cos(kSkySunRadius)};
		auto direction = glm::normalize(glm::vec3(kSkySunDirection[0], kSkySunDirection[1], kSkySunDirection[2]));
		sky.sunDirection = {direction.x, direction.y, direction.z};
		// the disk's radiance: its irradiance over its solid angle
		sky.sunRadiance = kSkySunIrradiance / (2.0F * detail::kPi * (1.0F - sky.sunCosRadius));

		// as bright on average as the ambient light was (by solid angle, over a grid of directions)
		constexpr uint32_t kWidth = 512;
		constexpr uint32_t kHeight = kWidth / 2;
		double sum = 0.0;
		double weight = 0.0;
		for (uint32_t y = 0; y < kHeight; y++)
			for (uint32_t x = 0; x < kWidth; x++)
			{
				auto d = detail::Direction(
					(static_cast<float>(x) + 0.5F) / static_cast<float>(kWidth), (static_cast<float>(y) + 0.5F) / static_cast<float>(kHeight));
				auto color = SkyRadiance(sky, {d.x, d.y, d.z});
				double w = detail::RowWeight(y, kHeight);
				sum += ((0.2126 * color[0]) + (0.7152 * color[1]) + (0.0722 * color[2])) * w;
				weight += w;
			}
		auto scale = static_cast<float>(0.3 / (sum / weight));
		for (auto* color : {&sky.zenith, &sky.horizon, &sky.ground})
			for (auto& channel : *color)
				channel *= scale;
		return sky;
	}();
	return kSky;
}

std::array<float, 3> SkyRadiance(const SkyParameters& sky, const std::array<float, 3>& direction)
{
	// the sky fades from the horizon up, the ground darkens towards the nadir, with a soft edge between them
	float up = direction[1];
	glm::vec3 zenith(sky.zenith[0], sky.zenith[1], sky.zenith[2]);
	glm::vec3 horizon(sky.horizon[0], sky.horizon[1], sky.horizon[2]);
	glm::vec3 groundColor(sky.ground[0], sky.ground[1], sky.ground[2]);
	glm::vec3 above = glm::mix(horizon, zenith, std::sqrt(std::max(up, 0.0F)));
	glm::vec3 below = groundColor * (1.0F - (0.4F * std::sqrt(std::max(-up, 0.0F))));
	glm::vec3 color = glm::mix(below, above, glm::smoothstep(-0.02F, 0.02F, up));
	return {color.x, color.y, color.z};
}

Panorama ProceduralSky(uint32_t width)
{
	ZoneScopedN("gfx::environment::ProceduralSky");

	const auto& sky = ProceduralSkyParameters();
	Panorama panorama{.width = width, .height = width / 2};
	panorama.rgb.resize(static_cast<size_t>(panorama.width) * panorama.height * 3);
	for (uint32_t y = 0; y < panorama.height; y++)
		for (uint32_t x = 0; x < panorama.width; x++)
		{
			auto d = detail::Direction(
				(static_cast<float>(x) + 0.5F) / static_cast<float>(panorama.width),
				(static_cast<float>(y) + 0.5F) / static_cast<float>(panorama.height));
			auto color = SkyRadiance(sky, {d.x, d.y, d.z});
			std::memcpy(&panorama.rgb[(static_cast<size_t>(y) * panorama.width + x) * 3], color.data(), sizeof(float) * 3);
		}

	// the sun: a disk, its texels covered in part where its edge crosses them (4x4 samples each), then scaled to give
	// exactly its irradiance (radiance times the texels' solid angles)
	auto sunDirection = glm::vec3(sky.sunDirection[0], sky.sunDirection[1], sky.sunDirection[2]);
	float cosRadius = sky.sunCosRadius;
	float cosReach = std::cos(kSkySunRadius + (4.0F * detail::kPi / static_cast<float>(panorama.height)));
	constexpr uint32_t kSamples = 4;
	std::vector<std::pair<size_t, float>> covered; // texel, coverage
	double irradiance = 0.0;
	for (uint32_t y = 0; y < panorama.height; y++)
		for (uint32_t x = 0; x < panorama.width; x++)
		{
			float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(panorama.width);
			float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(panorama.height);
			if (glm::dot(detail::Direction(u, v), sunDirection) < cosReach)
				continue;
			uint32_t inside = 0;
			for (uint32_t sy = 0; sy < kSamples; sy++)
				for (uint32_t sx = 0; sx < kSamples; sx++)
				{
					auto d = detail::Direction(
						(static_cast<float>(x) + ((static_cast<float>(sx) + 0.5F) / kSamples)) / static_cast<float>(panorama.width),
						(static_cast<float>(y) + ((static_cast<float>(sy) + 0.5F) / kSamples)) / static_cast<float>(panorama.height));
					inside += glm::dot(d, sunDirection) >= cosRadius ? 1U : 0U;
				}
			if (inside == 0)
				continue;
			float coverage = static_cast<float>(inside) / (kSamples * kSamples);
			covered.emplace_back((static_cast<size_t>(y) * panorama.width) + x, coverage);
			double solidAngle = (2.0 * std::numbers::pi / panorama.width) * (std::numbers::pi / panorama.height) * detail::RowWeight(y, panorama.height);
			irradiance += coverage * solidAngle;
		}
	auto radiance = static_cast<float>(kSkySunIrradiance / irradiance);
	for (auto [texel, coverage] : covered)
		for (uint32_t c = 0; c < 3; c++)
			panorama.rgb[(texel * 3) + c] += coverage * radiance;

	return panorama;
}

namespace detail
{

// the brightest dominant light left in a panorama (see ExtractDominantLights), taken out, or none. referenceMean: the
// mean luminance of the panorama before any was taken out, which the light's share is measured against
DominantLight ExtractDominantLight(Panorama& panorama, float referenceMean)
{
	// a dominant light is far brighter than the panorama's mean, and small: the texels around the brightest one, up to 15
	// degrees away, brighter than 2% of it (and than 10 times the mean): what is left of its glow below that stays in the
	// environment. it takes the radiance around it, and gives the rest as its irradiance, if that is at least 5% of what
	// the whole environment gives a surface facing it
	constexpr float kContrast = 50.0F;
	constexpr float kCore = 0.02F;
	constexpr float kMinimumThreshold = 10.0F;
	constexpr float kMaxRadius = 0.26F;
	constexpr float kMinimumShare = 0.05F;

	auto width = panorama.width;
	auto height = panorama.height;
	if (width == 0 || height == 0)
		return {};
	auto luminance = [&panorama](size_t texel)
	{
		const float* p = &panorama.rgb[texel * 3];
		return (0.2126F * p[0]) + (0.7152F * p[1]) + (0.0722F * p[2]);
	};
	auto solidAngle = [width, height](uint32_t y)
	{ return (2.0 * std::numbers::pi / width) * (std::numbers::pi / height) * detail::RowWeight(y, height); };
	auto direction = [width, height](size_t texel)
	{
		return detail::Direction(
			(static_cast<float>(texel % width) + 0.5F) / static_cast<float>(width),
			(static_cast<float>(texel / width) + 0.5F) / static_cast<float>(height));
	};

	double total = 0.0;
	size_t brightest = 0;
	float maxLuminance = 0.0F;
	for (uint32_t y = 0; y < height; y++)
		for (uint32_t x = 0; x < width; x++)
		{
			size_t texel = (static_cast<size_t>(y) * width) + x;
			auto l = luminance(texel);
			total += l * solidAngle(y);
			if (l > maxLuminance)
			{
				maxLuminance = l;
				brightest = texel;
			}
		}
	auto mean = static_cast<float>(total / (4.0 * std::numbers::pi));
	if (referenceMean <= 0.0F)
		referenceMean = mean;
	if (!(mean > 0.0F) || !(maxLuminance > kContrast * mean))
		return {};

	// the region: flood filled from the brightest texel (4-connected, around in u)
	float threshold = std::max(maxLuminance * kCore, kMinimumThreshold * mean);
	float cosMaxRadius = std::cos(kMaxRadius);
	auto center = direction(brightest);
	std::vector<uint8_t> inRegion(static_cast<size_t>(width) * height, 0);
	std::vector<size_t> region;
	std::vector<size_t> stack{brightest};
	inRegion[brightest] = 1;
	while (!stack.empty())
	{
		auto texel = stack.back();
		stack.pop_back();
		region.push_back(texel);
		auto x = static_cast<uint32_t>(texel % width);
		auto y = static_cast<uint32_t>(texel / width);
		std::array<std::pair<uint32_t, uint32_t>, 4> neighbors{{{(x + 1) % width, y}, {(x + width - 1) % width, y}, {x, y + 1}, {x, y - 1}}};
		for (auto [nx, ny] : neighbors)
		{
			if (ny >= height)
				continue;
			size_t next = (static_cast<size_t>(ny) * width) + nx;
			if (inRegion[next] == 0 && luminance(next) > threshold && glm::dot(direction(next), center) >= cosMaxRadius)
			{
				inRegion[next] = 1;
				stack.push_back(next);
			}
		}
	}

	// the radiance around it: the mean of the texels bordering the region
	glm::dvec3 ring(0.0);
	uint32_t ringCount = 0;
	for (auto texel : region)
	{
		auto x = static_cast<uint32_t>(texel % width);
		auto y = static_cast<uint32_t>(texel / width);
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++)
			{
				auto ny = static_cast<int64_t>(y) + dy;
				if (ny < 0 || ny >= height)
					continue;
				size_t next = (static_cast<size_t>(ny) * width) + ((x + width + dx) % width);
				if (inRegion[next] != 0)
					continue;
				const float* p = &panorama.rgb[next * 3];
				ring += glm::dvec3(p[0], p[1], p[2]);
				ringCount++;
			}
	}
	glm::vec3 background = ringCount > 0 ? glm::vec3(ring / static_cast<double>(ringCount)) : glm::vec3(mean);

	glm::dvec3 irradiance(0.0);
	glm::dvec3 weightedDirection(0.0);
	for (auto texel : region)
	{
		const float* p = &panorama.rgb[texel * 3];
		auto excess = glm::max(glm::dvec3(p[0], p[1], p[2]) - glm::dvec3(background), glm::dvec3(0.0));
		auto omega = solidAngle(static_cast<uint32_t>(texel / width));
		irradiance += excess * omega;
		weightedDirection += glm::dvec3(direction(texel)) * (glm::dot(excess, glm::dvec3(0.2126, 0.7152, 0.0722)) * omega);
	}
	auto irradianceLuminance = glm::dot(irradiance, glm::dvec3(0.2126, 0.7152, 0.0722));
	// what the environment gives a surface: pi times its mean radiance, for a uniform one
	if (irradianceLuminance < kMinimumShare * std::numbers::pi * referenceMean || glm::length(weightedDirection) <= 0.0)
		return {};

	for (auto texel : region)
		std::memcpy(&panorama.rgb[texel * 3], &background[0], sizeof(float) * 3);

	auto lightDirection = glm::normalize(glm::vec3(weightedDirection));
	return DominantLight{
		.direction = {lightDirection.x, lightDirection.y, lightDirection.z},
		.irradiance = {static_cast<float>(irradiance.x), static_cast<float>(irradiance.y), static_cast<float>(irradiance.z)}};
}

} // namespace detail

std::vector<DominantLight> ExtractDominantLights(Panorama& panorama, size_t maxCount)
{
	ZoneScopedN("gfx::environment::ExtractDominantLights");

	// each found in what the ones before left (by its brightest texel), against the whole panorama's mean
	auto mean = MeanRadiance(panorama);
	auto referenceMean = static_cast<float>((0.2126 * mean[0]) + (0.7152 * mean[1]) + (0.0722 * mean[2]));
	std::vector<DominantLight> lights;
	while (lights.size() < maxCount)
	{
		auto light = detail::ExtractDominantLight(panorama, referenceMean);
		if (!light)
			break;
		lights.push_back(light);
	}
	// by irradiance: the one found first has the brightest texel, but not necessarily the most light (a window's)
	auto luminance = [](const DominantLight& light)
	{ return (0.2126F * light.irradiance[0]) + (0.7152F * light.irradiance[1]) + (0.0722F * light.irradiance[2]); };
	std::ranges::stable_sort(lights, [&luminance](const DominantLight& a, const DominantLight& b) { return luminance(a) > luminance(b); });
	return lights;
}

std::array<float, 3> EvaluateIrradiance(const Environment& environment, const std::array<float, 3>& n)
{
	auto basis = detail::ShBasis(glm::vec3(n[0], n[1], n[2]));
	glm::vec3 result(0.0F);
	for (size_t i = 0; i < 9; i++)
		result += glm::vec3(environment.irradiance[i][0], environment.irradiance[i][1], environment.irradiance[i][2]) * basis[i];
	result = glm::max(result, glm::vec3(0.0F));
	return {result.x, result.y, result.z};
}

std::array<double, 3> MeanRadiance(const Panorama& panorama)
{
	glm::dvec3 sum(0.0);
	double weight = 0.0;
	for (uint32_t y = 0; y < panorama.height; y++)
	{
		double solidAngle = std::sin((static_cast<double>(y) + 0.5) / panorama.height * std::numbers::pi);
		for (uint32_t x = 0; x < panorama.width; x++)
		{
			const float* p = &panorama.rgb[(static_cast<size_t>(y) * panorama.width + x) * 3];
			sum += glm::dvec3(p[0], p[1], p[2]) * solidAngle;
			weight += solidAngle;
		}
	}
	sum /= weight;
	return {sum.x, sum.y, sum.z};
}

std::array<double, 3> MeanRadiance(const image::MipLevel& level, std::span<const std::byte> data)
{
	Panorama panorama{.width = level.width, .height = level.height};
	panorama.rgb.resize(static_cast<size_t>(level.width) * level.height * 3);
	const auto* in = reinterpret_cast<const uint16_t*>(data.data() + level.offset);
	for (size_t i = 0; i < static_cast<size_t>(level.width) * level.height; i++)
		for (size_t c = 0; c < 3; c++)
			panorama.rgb[(i * 3) + c] = glm::unpackHalf1x16(in[(i * 4) + c]);
	return MeanRadiance(panorama);
}

} // namespace gfx::environment
