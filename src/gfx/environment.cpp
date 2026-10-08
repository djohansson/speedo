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

glm::vec2 Coordinates(const glm::vec3& d)
{
	float u = 0.5F + (std::atan2(d.x, -d.z) / (2.0F * kPi));
	float v = std::acos(std::clamp(d.y, -1.0F, 1.0F)) / kPi;
	return {u, v};
}

// the sine of a panorama row's polar angle: its texels' solid angle relative to the equator's
float RowWeight(uint32_t y, uint32_t height)
{
	return std::sin((static_cast<float>(y) + 0.5F) / static_cast<float>(height) * kPi);
}

// a panorama and its mips, for filtered importance sampling: each texel the mean of the four above it by their solid
// angles (the rows nearer the poles cover less of the sphere), so that every mip keeps the panorama's mean
struct Pyramid
{
	std::vector<Panorama> levels;

	explicit Pyramid(const Panorama& panorama)
	{
		levels.push_back(panorama);
		while (levels.back().width > 8)
		{
			const auto& above = levels.back();
			Panorama level{.width = above.width / 2, .height = above.height / 2};
			level.rgb.resize(static_cast<size_t>(level.width) * level.height * 3);
			for (uint32_t y = 0; y < level.height; y++)
			{
				float top = RowWeight(2 * y, above.height);
				float bottom = RowWeight(2 * y + 1, above.height);
				float scale = 0.5F / (top + bottom);
				for (uint32_t x = 0; x < level.width; x++)
					for (uint32_t c = 0; c < 3; c++)
					{
						auto at = [&above, c](uint32_t ax, uint32_t ay) { return above.rgb[((static_cast<size_t>(ay) * above.width + ax) * 3) + c]; };
						level.rgb[((static_cast<size_t>(y) * level.width + x) * 3) + c] =
							scale * ((top * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y))) + (bottom * (at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1))));
					}
			}
			levels.push_back(std::move(level));
		}
	}

	// bilinear, wrapping around in u and clamped in v
	[[nodiscard]] glm::vec3 Sample(size_t levelIndex, glm::vec2 uv) const
	{
		const auto& level = levels[std::min(levelIndex, levels.size() - 1)];
		float x = (uv.x * static_cast<float>(level.width)) - 0.5F;
		float y = std::clamp((uv.y * static_cast<float>(level.height)) - 0.5F, 0.0F, static_cast<float>(level.height - 1));
		auto x0 = static_cast<int64_t>(std::floor(x));
		auto y0 = static_cast<int64_t>(std::floor(y));
		float fx = x - static_cast<float>(x0);
		float fy = y - static_cast<float>(y0);
		auto w = static_cast<int64_t>(level.width);
		auto texel = [&level, w](int64_t tx, int64_t ty)
		{
			tx = ((tx % w) + w) % w;
			ty = std::clamp<int64_t>(ty, 0, level.height - 1);
			const float* p = &level.rgb[static_cast<size_t>((ty * w + tx) * 3)];
			return glm::vec3(p[0], p[1], p[2]);
		};
		return glm::mix(
			glm::mix(texel(x0, y0), texel(x0 + 1, y0), fx), glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), fx), fy);
	}

	// trilinear between the mips
	[[nodiscard]] glm::vec3 Sample(float lod, glm::vec2 uv) const
	{
		lod = std::clamp(lod, 0.0F, static_cast<float>(levels.size() - 1));
		auto below = static_cast<size_t>(std::floor(lod));
		float fraction = lod - static_cast<float>(below);
		auto color = Sample(below, uv);
		return fraction > 0.0F ? glm::mix(color, Sample(below + 1, uv), fraction) : color;
	}
};

glm::vec2 Hammersley(uint32_t i, uint32_t count)
{
	uint32_t bits = i;
	bits = (bits << 16U) | (bits >> 16U);
	bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
	bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
	bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
	bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
	return {static_cast<float>(i) / static_cast<float>(count), static_cast<float>(bits) * 2.3283064365386963e-10F};
}

// the specular radiance of a direction n for a roughness: the panorama weighted by the GGX lobe around it (with n = v =
// r, as the split sum approximation assumes), sampled by importance from the pyramid's mips by each sample's solid angle
glm::vec3 PrefilterTexel(const Pyramid& pyramid, const glm::vec3& n, float roughness)
{
	constexpr uint32_t kSampleCount = 128;
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;

	glm::vec3 up = std::abs(n.y) < 0.999F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F);
	glm::vec3 tangent = glm::normalize(glm::cross(up, n));
	glm::vec3 bitangent = glm::cross(n, tangent);

	const auto& base = pyramid.levels.front();
	// a texel's solid angle at the equator, times its row's sine elsewhere
	float equatorTexelSolidAngle = (2.0F * kPi / static_cast<float>(base.width)) * (kPi / static_cast<float>(base.height));

	glm::vec3 sum(0.0F);
	float weight = 0.0F;
	for (uint32_t i = 0; i < kSampleCount; i++)
	{
		auto xi = Hammersley(i, kSampleCount);
		float cosTheta = std::sqrt((1.0F - xi.y) / (1.0F + ((alpha2 - 1.0F) * xi.y)));
		float sinTheta = std::sqrt(std::max(0.0F, 1.0F - (cosTheta * cosTheta)));
		float phi = 2.0F * kPi * xi.x;
		glm::vec3 h = (tangent * (sinTheta * std::cos(phi))) + (bitangent * (sinTheta * std::sin(phi))) + (n * cosTheta);
		glm::vec3 l = (2.0F * glm::dot(n, h) * h) - n;
		float nDotL = glm::dot(n, l);
		if (nDotL <= 0.0F)
			continue;

		// pdf of l: D(h) * nDotH / (4 vDotH), with v = n
		float d = (cosTheta * cosTheta * (alpha2 - 1.0F)) + 1.0F;
		float distribution = alpha2 / (kPi * d * d);
		float pdf = distribution / 4.0F;
		float sampleSolidAngle = 1.0F / (static_cast<float>(kSampleCount) * pdf + 1e-6F);
		auto uv = Coordinates(l);
		float texelSolidAngle = equatorTexelSolidAngle * std::max(std::sin(uv.y * kPi), 1.0F / static_cast<float>(base.height));
		float lod = 0.5F * std::log2(sampleSolidAngle / texelSolidAngle) + 1.0F;

		sum += pyramid.Sample(lod, uv) * nDotL;
		weight += nDotL;
	}
	return weight > 0.0F ? sum / weight : pyramid.Sample(0.0F, Coordinates(n));
}

// the sheen radiance of a direction n for a roughness: the panorama weighted by KHR_materials_sheen's Charlie lobe
// around it (n = v = r, as PrefilterTexel), D(h) n.l. its weight is near the horizon at low roughness, where importance
// sampling the half vectors (Estevez and Kulla) reflects most samples below it, so l is sampled over the hemisphere
// instead, with cos(theta) = u^k, which puts more of them near the horizon the lower the roughness
glm::vec3 PrefilterSheenTexel(const Pyramid& pyramid, const glm::vec3& n, float roughness)
{
	constexpr uint32_t kSampleCount = 512;
	float clamped = std::max(roughness, 0.07F);
	float inverseAlpha = 1.0F / (clamped * clamped);

	glm::vec3 up = std::abs(n.y) < 0.999F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F);
	glm::vec3 tangent = glm::normalize(glm::cross(up, n));
	glm::vec3 bitangent = glm::cross(n, tangent);

	const auto& base = pyramid.levels.front();
	float equatorTexelSolidAngle = (2.0F * kPi / static_cast<float>(base.width)) * (kPi / static_cast<float>(base.height));
	float exponent = 1.0F + (0.1F * inverseAlpha);

	glm::vec3 sum(0.0F);
	float weight = 0.0F;
	for (uint32_t i = 0; i < kSampleCount; i++)
	{
		auto xi = Hammersley(i, kSampleCount);
		float cosTheta = std::pow(1.0F - xi.y, exponent);
		float sinTheta = std::sqrt(std::max(0.0F, 1.0F - (cosTheta * cosTheta)));
		float phi = 2.0F * kPi * xi.x;
		glm::vec3 l = (tangent * (sinTheta * std::cos(phi))) + (bitangent * (sinTheta * std::sin(phi))) + (n * cosTheta);

		// D(h) is sin(theta_h)^(1 / alpha) up to a constant, and theta_h = theta_l / 2: relative to its maximum on the
		// hemisphere (theta_h = 45 degrees), so that it doesn't underflow
		float sinHalf = std::sqrt(std::max(0.0F, (1.0F - cosTheta) * 0.5F));
		// over the density of cos(theta), u^k's: c^(1 / k - 1) / k
		float pdf = std::pow(cosTheta, (1.0F / exponent) - 1.0F) / exponent;
		float w = std::pow(sinHalf * std::numbers::sqrt2_v<float>, inverseAlpha) * cosTheta / pdf;
		if (!(w > 0.0F))
			continue;
		float sampleSolidAngle = 2.0F * kPi / (static_cast<float>(kSampleCount) * pdf);

		auto uv = Coordinates(l);
		float texelSolidAngle = equatorTexelSolidAngle * std::max(std::sin(uv.y * kPi), 1.0F / static_cast<float>(base.height));
		float lod = 0.5F * std::log2(sampleSolidAngle / texelSolidAngle) + 1.0F;

		sum += pyramid.Sample(lod, uv) * w;
		weight += w;
	}
	return weight > 0.0F ? sum / weight : pyramid.Sample(0.0F, Coordinates(n));
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

Panorama ProceduralSky(uint32_t width)
{
	ZoneScopedN("gfx::environment::ProceduralSky");

	Panorama panorama{.width = width, .height = width / 2};
	panorama.rgb.resize(static_cast<size_t>(panorama.width) * panorama.height * 3);
	const glm::vec3 kZenith(0.25F, 0.40F, 0.75F);
	const glm::vec3 kHorizon(0.80F, 0.82F, 0.85F);
	const glm::vec3 kGround(0.30F, 0.27F, 0.24F);
	for (uint32_t y = 0; y < panorama.height; y++)
		for (uint32_t x = 0; x < panorama.width; x++)
		{
			auto d = detail::Direction(
				(static_cast<float>(x) + 0.5F) / static_cast<float>(panorama.width),
				(static_cast<float>(y) + 0.5F) / static_cast<float>(panorama.height));
			// the sky fades from the horizon up, the ground darkens towards the nadir, with a soft edge between them
			glm::vec3 sky = glm::mix(kHorizon, kZenith, std::pow(std::max(d.y, 0.0F), 0.5F));
			glm::vec3 ground = kGround * (1.0F - (0.4F * std::pow(std::max(-d.y, 0.0F), 0.5F)));
			glm::vec3 color = glm::mix(ground, sky, glm::smoothstep(-0.02F, 0.02F, d.y));
			std::memcpy(&panorama.rgb[(static_cast<size_t>(y) * panorama.width + x) * 3], &color[0], sizeof(float) * 3);
		}

	// as bright on average as the ambient light was
	auto mean = MeanRadiance(panorama);
	auto luminance = (0.2126 * mean[0]) + (0.7152 * mean[1]) + (0.0722 * mean[2]);
	auto scale = static_cast<float>(0.3 / luminance);
	for (auto& value : panorama.rgb)
		value *= scale;

	// the sun: a disk, its texels covered in part where its edge crosses them (4x4 samples each), then scaled to give
	// exactly its irradiance (radiance times the texels' solid angles)
	auto sunDirection = glm::normalize(glm::vec3(kSkySunDirection[0], kSkySunDirection[1], kSkySunDirection[2]));
	float cosRadius = std::cos(kSkySunRadius);
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

std::expected<Environment, std::string> Prefilter(
	const Panorama& panorama, const std::function<std::byte*(size_t size)>& allocate, const std::function<bool()>& cancelled)
{
	ZoneScopedN("gfx::environment::Prefilter");

	if (panorama.width < (8U << (kLevelCount - 1)) / 4 || panorama.width != 2 * panorama.height)
		return std::unexpected(std::format("a panorama of {}x{} is too small to prefilter", panorama.width, panorama.height));

	// the lighting is the panorama without its dominant light, which lights the scene as a directional light instead (level 0 keeps
	// it: the backdrop, and mirrors)
	Panorama lighting = panorama;
	Environment environment;
	environment.dominantLights = ExtractDominantLights(lighting);
	detail::Pyramid pyramid(lighting);

	uint32_t offset = 0;
	for (uint32_t levelIt = 0; levelIt < kLevelCount; levelIt++)
	{
		uint32_t width = std::max(panorama.width >> levelIt, 2U);
		uint32_t height = std::max(width / 2, 1U);
		uint32_t size = width * height * 4 * sizeof(uint16_t);
		environment.levels.push_back({.width = width, .height = height, .offset = offset, .size = size});
		offset += size;
	}
	for (uint32_t levelIt = 0; levelIt < kLevelCount; levelIt++)
	{
		uint32_t width = std::max((panorama.width / 8) >> levelIt, 2U);
		uint32_t height = std::max(width / 2, 1U);
		uint32_t size = width * height * 4 * sizeof(uint16_t);
		environment.sheenLevels.push_back({.width = width, .height = height, .offset = offset, .size = size});
		offset += size;
	}
	environment.size = offset;

	auto* memory = allocate(environment.size);
	for (uint32_t levelIt = 0; levelIt < 2 * kLevelCount; levelIt++)
	{
		if (cancelled && cancelled())
			return std::unexpected("cancelled");

		bool sheen = levelIt >= kLevelCount;
		const auto& level = sheen ? environment.sheenLevels[levelIt - kLevelCount] : environment.levels[levelIt];
		float roughness = static_cast<float>(levelIt % kLevelCount) / static_cast<float>(kLevelCount - 1);
		auto* out = reinterpret_cast<uint16_t*>(memory + level.offset);
		std::vector<uint32_t> rows(level.height);
		std::iota(rows.begin(), rows.end(), 0U);
		std::for_each(
			std::execution::par,
			rows.begin(),
			rows.end(),
			[&](uint32_t y)
			{
				for (uint32_t x = 0; x < level.width; x++)
				{
					glm::vec2 uv(
						(static_cast<float>(x) + 0.5F) / static_cast<float>(level.width),
						(static_cast<float>(y) + 0.5F) / static_cast<float>(level.height));
					auto direction = detail::Direction(uv.x, uv.y);
					// level 0 is the panorama as it is (the same size)
					auto original = [&panorama, x, y]
					{
						const float* p = &panorama.rgb[((static_cast<size_t>(y) * panorama.width) + x) * 3];
						return glm::vec3(p[0], p[1], p[2]);
					};
					auto color = sheen		   ? detail::PrefilterSheenTexel(pyramid, direction, roughness)
								 : levelIt == 0 ? original()
												: detail::PrefilterTexel(pyramid, direction, roughness);
					auto* texel = &out[(static_cast<size_t>(y) * level.width + x) * 4];
					texel[0] = glm::packHalf1x16(color.x);
					texel[1] = glm::packHalf1x16(color.y);
					texel[2] = glm::packHalf1x16(color.z);
					texel[3] = glm::packHalf1x16(1.0F);
				}
			});
	}

	// the irradiance: the radiance projected on the basis (from a mip of the pyramid, by each texel's solid angle),
	// convolved with the clamped cosine (Ramamoorthi and Hanrahan: pi, 2pi/3 and pi/4 per band), divided by pi
	const auto& source = *std::ranges::find_if(pyramid.levels, [](const Panorama& level) { return level.width <= 128; });
	std::array<glm::dvec3, 9> coefficients{};
	for (uint32_t y = 0; y < source.height; y++)
	{
		float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(source.height);
		double solidAngle = (2.0 * std::numbers::pi / source.width) * (std::numbers::pi / source.height) * std::sin(v * detail::kPi);
		for (uint32_t x = 0; x < source.width; x++)
		{
			auto d = detail::Direction((static_cast<float>(x) + 0.5F) / static_cast<float>(source.width), v);
			const float* p = &source.rgb[(static_cast<size_t>(y) * source.width + x) * 3];
			auto basis = detail::ShBasis(d);
			for (size_t i = 0; i < 9; i++)
				coefficients[i] += glm::dvec3(p[0], p[1], p[2]) * static_cast<double>(basis[i]) * solidAngle;
		}
	}
	constexpr std::array<double, 9> kBand{1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25};
	for (size_t i = 0; i < 9; i++)
		environment.irradiance[i] = {
			static_cast<float>(coefficients[i].x * kBand[i]),
			static_cast<float>(coefficients[i].y * kBand[i]),
			static_cast<float>(coefficients[i].z * kBand[i]),
			0.0F};

	return environment;
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
