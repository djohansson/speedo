#include "environment.h"

#include <gfx/shaders/capi.h>

#include <core/file.h>
#include <core/profiling.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <execution>
#include <format>
#include <memory>
#include <numbers>
#include <numeric>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/type_ptr.hpp>

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
	auto file = core::file::Map(path);
	if (!file)
		return std::unexpected(std::format("{}: can't read the panorama: {}", path.string(), file.error().message()));
	return Import(std::span<const std::byte>(file->data(), file->size()), path.string(), maxWidth);
}

std::expected<Panorama, std::string> Import(std::span<const std::byte> data, std::string_view name, uint32_t maxWidth)
{
	ZoneScopedN("gfx::environment::Import");

	int width = 0;
	int height = 0;
	int channelCount = 0;
	std::unique_ptr<float, decltype(&stbi_image_free)> pixels(
		stbi_loadf_from_memory(
			reinterpret_cast<const stbi_uc*>(data.data()), static_cast<int>(data.size()), &width, &height, &channelCount, 3),
		&stbi_image_free);
	if (!pixels)
		return std::unexpected(std::format("{}: can't read the panorama: {}", name, stbi_failure_reason()));
	if (width != 2 * height)
		return std::unexpected(std::format("{}: a panorama is twice as wide as high, not {}x{}", name, width, height));

	Panorama panorama{.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)};
	if (panorama.width > maxWidth)
	{
		panorama.width = maxWidth;
		panorama.height = maxWidth / 2;
		panorama.rgb.resize(static_cast<size_t>(panorama.width) * panorama.height * 3);
		if (stbir_resize_float_linear(
				pixels.get(), width, height, 0, panorama.rgb.data(), static_cast<int>(panorama.width), static_cast<int>(panorama.height), 0,
				STBIR_RGB) == nullptr)
			return std::unexpected(std::format("{}: can't scale the panorama", name));
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

glm::vec3 SunDirection(float latitudeDegrees, float dayOfYear, float timeOfDay)
{
	float latitude = glm::radians(latitudeDegrees);
	float declination = glm::radians(-23.44F) * std::cos(2.0F * detail::kPi / 365.0F * (dayOfYear + 10.0F));
	float hourAngle = glm::radians(15.0F * (timeOfDay - 12.0F));
	float east = -std::cos(declination) * std::sin(hourAngle);
	float north = (std::cos(latitude) * std::sin(declination)) - (std::sin(latitude) * std::cos(declination) * std::cos(hourAngle));
	float up = (std::sin(latitude) * std::sin(declination)) + (std::cos(latitude) * std::cos(declination) * std::cos(hourAngle));
	return glm::normalize(glm::vec3(east, up, -north));
}

float SkyAerosols(float visibility)
{
	// the extinction at the ground (at 550 nm) the visibility means, less the air's own: the aerosols', in Hillaire's
	double extinction = 3.912 / std::max(static_cast<double>(visibility), 1e-3);
	return static_cast<float>(std::max(extinction - SHADER_TYPES_ATMOSPHERE_RAYLEIGH_G, 0.0) / SHADER_TYPES_ATMOSPHERE_MIE_EXTINCTION);
}

glm::vec3 SunTransmittance(const glm::vec3& direction, float aerosols)
{
	// in doubles, km from the planet's center (as sky.h's SkyMediumAt and EnvironmentSkyTransmittance, which integrate
	// the same optical depth in floats)
	constexpr double kBottom = SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS;
	constexpr double kTop = SHADER_TYPES_ATMOSPHERE_TOP_RADIUS;
	glm::dvec3 origin(0.0, kBottom + SHADER_TYPES_ATMOSPHERE_EYE_ALTITUDE, 0.0);
	glm::dvec3 d = glm::normalize(glm::dvec3(direction));
	auto sphere = [&origin, &d](double radius)
	{
		double b = glm::dot(origin, d);
		double discriminant = (b * b) - (glm::dot(origin, origin) - (radius * radius));
		if (discriminant < 0.0)
			return -1.0;
		double root = std::sqrt(discriminant);
		return -b - root >= 0.0 ? -b - root : (-b + root >= 0.0 ? -b + root : -1.0);
	};
	if (sphere(kBottom) >= 0.0)
		return glm::vec3(0.0F);
	double end = sphere(kTop);
	constexpr int kStepCount = 256;
	double step = end / kStepCount;
	glm::dvec3 depth(0.0);
	for (int stepIt = 0; stepIt < kStepCount; stepIt++)
	{
		double altitude = glm::length(origin + (d * ((stepIt + 0.5) * step))) - kBottom;
		double rayleigh = std::exp(-altitude / SHADER_TYPES_ATMOSPHERE_RAYLEIGH_SCALE_HEIGHT);
		double mie = std::exp(-altitude / SHADER_TYPES_ATMOSPHERE_MIE_SCALE_HEIGHT) * aerosols;
		double ozone = std::max(0.0, 1.0 - (std::abs(altitude - SHADER_TYPES_ATMOSPHERE_OZONE_CENTER) / SHADER_TYPES_ATMOSPHERE_OZONE_HALF_WIDTH));
		depth += (glm::dvec3(SHADER_TYPES_ATMOSPHERE_RAYLEIGH_R, SHADER_TYPES_ATMOSPHERE_RAYLEIGH_G, SHADER_TYPES_ATMOSPHERE_RAYLEIGH_B) * rayleigh +
				  glm::dvec3(SHADER_TYPES_ATMOSPHERE_MIE_EXTINCTION * mie) +
				  glm::dvec3(SHADER_TYPES_ATMOSPHERE_OZONE_R, SHADER_TYPES_ATMOSPHERE_OZONE_G, SHADER_TYPES_ATMOSPHERE_OZONE_B) * ozone) *
				 step;
	}
	return glm::vec3(glm::exp(-depth));
}

DominantLight SkySun(const glm::vec3& direction, float aerosols)
{
	DominantLight sun{.direction = glm::normalize(direction), .angularRadius = kSkySunRadius};
	sun.irradiance = static_cast<float>(SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE) * SunTransmittance(sun.direction, aerosols);
	return sun;
}

namespace
{

// days since J2000.0 (2000 January 1st, noon): the calendar's years to the year's first day (a leap day every fourth year;
// 2100's missing one is a day off past it), then the day and the time (local solar time standing in for universal time:
// the longitude's hour or so moves the moon half a degree)
[[nodiscard]] double DaysSinceJ2000(float dayOfYear, float timeOfDay, int year)
{
	auto floorDiv4 = [](int value) { return value >= 0 ? value / 4 : -((-value + 3) / 4); };
	double yearDays = (365.0 * (year - 2000)) + floorDiv4(year - 1) - 499;
	return yearDays + (static_cast<double>(dayOfYear) - 1.0) + (static_cast<double>(timeOfDay) / 24.0) - 0.5;
}

// the sun's ecliptic longitude (degrees) by the days since J2000.0: its mean longitude and the equation of the centre
[[nodiscard]] double SunLongitude(double days)
{
	double meanAnomaly = glm::radians(357.528 + (0.9856003 * days));
	return 280.460 + (0.9856474 * days) + (1.915 * std::sin(meanAnomaly)) + (0.020 * std::sin(2.0 * meanAnomaly));
}

[[nodiscard]] double Obliquity(double days)
{
	return glm::radians(23.439 - (0.0000004 * days));
}

// right ascension and declination (radians) of an ecliptic longitude and latitude (degrees)
void Equatorial(double longitude, double latitude, double obliquity, double& ascension, double& declination)
{
	double lambda = glm::radians(longitude);
	double beta = glm::radians(latitude);
	double x = std::cos(beta) * std::cos(lambda);
	double y = (std::cos(obliquity) * std::cos(beta) * std::sin(lambda)) - (std::sin(obliquity) * std::sin(beta));
	double z = (std::sin(obliquity) * std::cos(beta) * std::sin(lambda)) + (std::cos(obliquity) * std::sin(beta));
	ascension = std::atan2(y, x);
	declination = std::asin(std::clamp(z, -1.0, 1.0));
}

// toward a body at an hour angle and declination from a latitude (radians), as SunDirection's frame
[[nodiscard]] glm::vec3 TowardBody(double latitude, double hourAngle, double declination)
{
	double east = -std::cos(declination) * std::sin(hourAngle);
	double north = (std::cos(latitude) * std::sin(declination)) - (std::sin(latitude) * std::cos(declination) * std::cos(hourAngle));
	double up = (std::sin(latitude) * std::sin(declination)) + (std::cos(latitude) * std::cos(declination) * std::cos(hourAngle));
	return glm::normalize(glm::vec3(east, up, -north));
}

} // namespace

float LocalSiderealTime(float dayOfYear, float timeOfDay, int year)
{
	double days = DaysSinceJ2000(dayOfYear, timeOfDay, year);
	double ascension = 0.0;
	double declination = 0.0;
	Equatorial(SunLongitude(days), 0.0, Obliquity(days), ascension, declination);
	double hourAngle = glm::radians(15.0 * (static_cast<double>(timeOfDay) - 12.0));
	return static_cast<float>(std::fmod((hourAngle + ascension) + (4.0 * glm::pi<double>()), 2.0 * glm::pi<double>()));
}

Moon MoonAt(float latitudeDegrees, float dayOfYear, float timeOfDay, int year)
{
	double days = DaysSinceJ2000(dayOfYear, timeOfDay, year);
	double meanLongitude = 218.316 + (13.176396 * days);
	double meanAnomaly = glm::radians(134.963 + (13.064993 * days));
	double node = glm::radians(93.272 + (13.229350 * days));
	double longitude = meanLongitude + (6.289 * std::sin(meanAnomaly));
	double latitude = 5.128 * std::sin(node);
	double ascension = 0.0;
	double declination = 0.0;
	Equatorial(longitude, latitude, Obliquity(days), ascension, declination);
	double hourAngle = static_cast<double>(LocalSiderealTime(dayOfYear, timeOfDay, year)) - ascension;
	Moon moon{.direction = TowardBody(glm::radians(static_cast<double>(latitudeDegrees)), hourAngle, declination)};
	auto sun = SunDirection(latitudeDegrees, dayOfYear, timeOfDay);
	moon.elongation = std::acos(std::clamp(glm::dot(moon.direction, sun), -1.0F, 1.0F));
	// Allen's: by the phase angle (sun, moon, earth: 180 degrees less the elongation), 0 at full
	double phaseAngle = 180.0 - glm::degrees(static_cast<double>(moon.elongation));
	double magnitudes = (0.026 * phaseAngle) + (4e-9 * phaseAngle * phaseAngle * phaseAngle * phaseAngle);
	moon.illuminance = static_cast<float>(SHADER_TYPES_ATMOSPHERE_MOON_ILLUMINANCE * std::pow(10.0, -0.4 * magnitudes));
	return moon;
}

DominantLight SkyMoon(const Moon& moon, float aerosols)
{
	DominantLight light{.direction = glm::normalize(moon.direction), .angularRadius = kSkySunRadius};
	light.irradiance = moon.illuminance * SunTransmittance(light.direction, aerosols);
	return light;
}

int CurrentYear()
{
	auto now = std::chrono::system_clock::now();
	auto days = std::chrono::floor<std::chrono::days>(now);
	return static_cast<int>(std::chrono::year_month_day(days).year());
}

void AddSun(Panorama& panorama, const DominantLight& sun)
{
	auto sunDirection = glm::normalize(sun.direction);
	float cosRadius = std::cos(kSkySunRadius);
	float cosReach = std::cos(kSkySunRadius + (4.0F * detail::kPi / static_cast<float>(panorama.height)));
	constexpr uint32_t kSamples = 4;
	std::vector<std::pair<size_t, float>> covered; // texel, coverage
	double solidAngles = 0.0;
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
			solidAngles += coverage * solidAngle;
		}
	if (!(solidAngles > 0.0))
		return;
	auto radiance = sun.irradiance / static_cast<float>(solidAngles);
	for (auto [texel, coverage] : covered)
		for (uint32_t c = 0; c < 3; c++)
			panorama.rgb[(texel * 3) + c] += coverage * radiance[static_cast<glm::length_t>(c)];
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
	double weightSum = 0.0;
	for (auto texel : region)
	{
		const float* p = &panorama.rgb[texel * 3];
		auto excess = glm::max(glm::dvec3(p[0], p[1], p[2]) - glm::dvec3(background), glm::dvec3(0.0));
		auto omega = solidAngle(static_cast<uint32_t>(texel / width));
		irradiance += excess * omega;
		auto weight = glm::dot(excess, glm::dvec3(0.2126, 0.7152, 0.0722)) * omega;
		weightedDirection += glm::dvec3(direction(texel)) * weight;
		weightSum += weight;
	}
	auto irradianceLuminance = glm::dot(irradiance, glm::dvec3(0.2126, 0.7152, 0.0722));
	// what the environment gives a surface: pi times its mean radiance, for a uniform one
	if (irradianceLuminance < kMinimumShare * std::numbers::pi * referenceMean || glm::length(weightedDirection) <= 0.0)
		return {};

	for (auto texel : region)
		std::memcpy(&panorama.rgb[texel * 3], &background[0], sizeof(float) * 3);

	auto lightDirection = glm::normalize(glm::vec3(weightedDirection));
	// its angular radius: a uniform disk of radius r has a mean squared angle about its center of r^2 / 2, so r is the
	// square root of twice the light's (weighed by its light), at least half a texel (the panorama's resolution)
	double spread = 0.0;
	for (auto texel : region)
	{
		const float* p = &panorama.rgb[texel * 3];
		auto excess = glm::max(glm::dvec3(p[0], p[1], p[2]) - glm::dvec3(background), glm::dvec3(0.0));
		auto weight = glm::dot(excess, glm::dvec3(0.2126, 0.7152, 0.0722)) * solidAngle(static_cast<uint32_t>(texel / width));
		auto angle = std::acos(std::clamp(static_cast<double>(glm::dot(direction(texel), lightDirection)), -1.0, 1.0));
		spread += weight * angle * angle;
	}
	auto halfTexel = std::numbers::pi / static_cast<double>(height) * 0.5;
	auto angularRadius = std::max(std::sqrt(2.0 * spread / std::max(weightSum, 1e-30)), halfTexel);
	return DominantLight{.direction = lightDirection, .irradiance = glm::vec3(irradiance), .angularRadius = static_cast<float>(angularRadius)};
}

} // namespace detail

std::vector<DominantLight> ExtractDominantLights(Panorama& panorama, size_t maxCount)
{
	ZoneScopedN("gfx::environment::ExtractDominantLights");

	// each found in what the ones before left (by its brightest texel), against the whole panorama's mean
	auto mean = MeanRadiance(panorama);
	auto referenceMean = static_cast<float>(glm::dot(mean, glm::dvec3(0.2126, 0.7152, 0.0722)));
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
	{ return glm::dot(light.irradiance, glm::vec3(0.2126F, 0.7152F, 0.0722F)); };
	std::ranges::stable_sort(lights, [&luminance](const DominantLight& a, const DominantLight& b) { return luminance(a) > luminance(b); });
	return lights;
}

glm::vec3 EvaluateIrradiance(const Environment& environment, const glm::vec3& n)
{
	auto basis = detail::ShBasis(n);
	glm::vec3 result(0.0F);
	for (size_t i = 0; i < 9; i++)
		result += glm::make_vec3(environment.irradiance[i].data()) * basis[i];
	return glm::max(result, glm::vec3(0.0F));
}

glm::dvec3 MeanRadiance(const Panorama& panorama)
{
	glm::dvec3 sum(0.0);
	double weight = 0.0;
	for (uint32_t y = 0; y < panorama.height; y++)
	{
		double solidAngle = std::sin((static_cast<double>(y) + 0.5) / panorama.height * std::numbers::pi);
		for (uint32_t x = 0; x < panorama.width; x++)
		{
			const float* p = &panorama.rgb[(static_cast<size_t>(y) * panorama.width + x) * 3];
			sum += glm::dvec3(glm::make_vec3(p)) * solidAngle;
			weight += solidAngle;
		}
	}
	return sum / weight;
}

glm::dvec3 MeanRadiance(const image::MipLevel& level, std::span<const std::byte> data)
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
