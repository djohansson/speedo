#pragma once

#include <gfx/imageimport.h>
#include <gfx/shaders/capi.h>

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// the environment the scene is lit by (image based lighting): a panorama of the radiance around it, prefiltered for the
// glTF brdf (see Shade in the shaders). cpu only, as the importers.
namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE::environment
{

// an equirectangular panorama of radiance, linear rgb, row by row: direction d is at u = 0.5 + atan2(d.x, -d.z) / 2pi
// (-z, where the cameras look by default, in the middle) and v = acos(d.y) / pi (+y up, the top row)
struct Panorama
{
	uint32_t width = 0; // twice the height
	uint32_t height = 0;
	std::vector<float> rgb;
};

// the panorama's widths: a file's is scaled down to this (a 8k panorama is 256 MB as half floats)
constexpr uint32_t kMaxFileWidth = 2048;
constexpr uint32_t kSkyWidth = 256;

// reads a panorama (Radiance .hdr, or anything else stb_image reads, as linear), scaled down to at most maxWidth
[[nodiscard]] std::expected<Panorama, std::string> Import(const std::filesystem::path& path, uint32_t maxWidth = kMaxFileWidth);
// the same, from a file's bytes (name is for messages)
[[nodiscard]] std::expected<Panorama, std::string> Import(
	std::span<const std::byte> data, std::string_view name, uint32_t maxWidth = kMaxFileWidth);

// radians: the real sun's (0.2665 degrees: 0.533 across), for its disk in the sky and its shadows' penumbrae
constexpr float kSkySunRadius = 0.00465F;

// the sky, the default environment where no file is loaded: Earth's atmosphere lit by the sun (see shaders/sky.h; the gpu
// draws it, its panorama kSkyWidth wide: what lights the scene is blurry, the prefiltered levels and the irradiance, and
// the shader draws what is seen sharply itself), the sun where the time of day puts it, at a latitude on a day of the
// year. the defaults: Paris (Bistro's), the 21st of June, half past ten in the morning (local solar time): 59 degrees up
// in the south east, where the sky's sun was before it moved
constexpr float kSkyDefaultLatitude = 48.86F; // degrees, north positive
constexpr float kSkyDefaultDayOfYear = 172.0F; // 1 the 1st of January
constexpr float kSkyDefaultTimeOfDay = 10.5F; // hours of local solar time: 12 the sun highest

// toward the sun (a unit vector in the panorama's space: +x east, +y up, -z north, where the cameras look by default),
// at a latitude (degrees), a day of the year and a time of day (hours): the declination by the day (Cooper's), the hour
// angle by the time (15 degrees an hour from noon; no equation of time)
[[nodiscard]] glm::vec3 SunDirection(float latitudeDegrees, float dayOfYear, float timeOfDay);

// how clear the sky's air is: how far one sees through it at the ground, its meteorological visibility in km (where a
// dark object against the horizon keeps 2% of its contrast: Koschmieder's 3.912 over the extinction, at 550 nm, the
// green channel's), which sets how much of the aerosols it has: Hillaire's at kSkyDefaultVisibility (a very clear day),
// none from kSkyClearestVisibility (the air's own scattering alone), more the hazier (20 km a hazy day, 2 km mist)
constexpr float kSkyDefaultVisibility =
	static_cast<float>(3.912 / (SHADER_TYPES_ATMOSPHERE_RAYLEIGH_G + SHADER_TYPES_ATMOSPHERE_MIE_EXTINCTION));
constexpr float kSkyClearestVisibility = static_cast<float>(3.912 / SHADER_TYPES_ATMOSPHERE_RAYLEIGH_G);
// the aerosols for a visibility, times Hillaire's (EnvironmentData::skyAtmosphere's x)
[[nodiscard]] float SkyAerosols(float visibility);

// the fog near the ground (see AerialPerspectiveData::fog), a layer of haze and droplets on top of the sky's air, densest
// at the ground: a clear day's by default, 30 km of visibility at the ground (a clear day's is 20 to 40, the sky's own air
// very clear above it, see kSkyDefaultVisibility) in a layer of 200 m scale height (the air near the surface, where the
// day's haze and moisture gather: an optical depth of 0.03 up through it, a clean day's aerosols), which hazes what is far
// along the ground and the horizon's lowest degrees, and leaves the sky above and the sun clear (5% of it taken at 30
// degrees up). 50 m drew its horizon a line a pixel thick, darker than the sky above and the ground below. mist and fog
// are hundreds of metres of visibility, and layers of tens of metres what the light shafts of a building's scale shine
// through
constexpr float kFogDefaultVisibility = 30.0F; // km
constexpr float kFogDefaultHeight = 200.0F; // m

// the atmosphere's transmittance toward a direction (a unit vector, +y up), from the eye's altitude
// (SHADER_TYPES_ATMOSPHERE_EYE_ALTITUDE) to the top of the atmosphere, as the sky's transmittance table has it: 0 where the
// planet is in the way (the sun below the horizon). aerosols: see SkyAerosols
[[nodiscard]] glm::vec3 SunTransmittance(const glm::vec3& direction, float aerosols = 1.0F);

// a panorama's dominant light: a strong compact light source (the sun, a lamp, a window), which lights the scene
// as a directional light (with shadows) instead of as part of the environment. direction: towards it (in the
// panorama's space), irradiance: what it gives a surface facing it, by color (radiance times solid angle, so in lux for
// a panorama in nits). none (zero irradiance) for a panorama without one (overcast, interiors lit evenly)
struct DominantLight
{
	glm::vec3 direction{0.0F, 1.0F, 0.0F};
	glm::vec3 irradiance{0.0F};
	// radians: the radius of a uniform disk spread as it is (its light's angular spread about direction), for the
	// penumbrae of its shadows (LightData::size)
	float angularRadius = 0.0F;

	[[nodiscard]] explicit operator bool() const noexcept { return irradiance[0] > 0.0F || irradiance[1] > 0.0F || irradiance[2] > 0.0F; }
};

// the sky's sun toward a direction (see SunDirection), as a directional light: the sun's illuminance at the top of the
// atmosphere (SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE) through it (SunTransmittance, with its aerosols), none below the
// horizon
[[nodiscard]] DominantLight SkySun(const glm::vec3& direction, float aerosols = 1.0F);

// the moon where the time puts it: toward it (in SunDirection's frame), its illuminance at the top of the atmosphere at its
// phase (full SHADER_TYPES_ATMOSPHERE_MOON_ILLUMINANCE, a quarter about a twelfth of it: Allen's magnitudes, 0.026 a
// degree of phase angle and 4e-9 its fourth power), and its elongation from the sun (radians: pi at full)
struct Moon
{
	glm::vec3 direction{0.0F, 1.0F, 0.0F};
	float illuminance = 0.0F;
	float elongation = 0.0F;
};

// the moon at a latitude on a day of a year at a time of day (as SunDirection): a low precision ephemeris (Schlyter's,
// Meeus's leading terms, within a degree or two: the moon's mean longitude, anomaly and node, the sun's, the obliquity),
// the hour angle by the local sidereal time, so that the moon is where it is relative to the sun
[[nodiscard]] Moon MoonAt(float latitudeDegrees, float dayOfYear, float timeOfDay, int year);

// the local sidereal time (radians) at a day of a year and a local solar time (the sun's hour angle plus its right
// ascension): the stars' frame (see SkyStars)
[[nodiscard]] float LocalSiderealTime(float dayOfYear, float timeOfDay, int year);

// the moon as a directional light (as SkySun): its illuminance through the atmosphere, none below the horizon
[[nodiscard]] DominantLight SkyMoon(const Moon& moon, float aerosols = 1.0F);

// the calendar year now, the year's default
[[nodiscard]] int CurrentYear();

// a sun's disk (kSkySunRadius) added to a panorama, its texels covered in part where its edge crosses them (4x4 samples
// each), giving exactly its irradiance: for tests of the dominant lights' extraction
void AddSun(Panorama& panorama, const DominantLight& sun);

// at most this many dominant lights are taken out of a panorama
constexpr size_t kMaxDominantLights = 4;

// finds a panorama's dominant lights (see DominantLight), the most irradiance first, and takes them out: their texels get the
// radiance around them. each is far brighter than what is left of the panorama, and gives at least 5% of what the
// whole panorama gives a surface facing it
[[nodiscard]] std::vector<DominantLight> ExtractDominantLights(Panorama& panorama, size_t maxCount = kMaxDominantLights);

// prefiltered levels: level i is the panorama filtered for roughness i / (kLevelCount - 1), half the size of the one
// before (level 0, roughness 0, is the panorama itself, dominant lights included: the backdrop, and mirrors). the other
// levels, the sheen levels and the irradiance are of the panorama without its dominant lights (see
// ExtractDominantLights), which light the scene as directional lights instead
constexpr uint32_t kLevelCount = 6;

// a panorama prefiltered for the shader (by the kernels of environment.slang: gfx::EnvironmentFilter runs them on the gpu,
// the tests on the cpu, see src/tools/environmentkernels.h): the specular levels, as R16G16B16A16 half floats, which are
// the mip levels of
// one texture (sampled at the roughness's level), and the irradiance as 9 spherical harmonics coefficients (rgb, w
// unused), divided by pi: what a white Lambertian surface of normal n reflects, sum(irradiance[i] * Y_i(n)), with the
// basis' constants applied as in EvaluateIrradiance
struct Environment
{
	std::vector<image::MipLevel> levels;
	// the same for KHR_materials_sheen's Charlie lobe (kLevelCount levels, for roughness i / (kLevelCount - 1), from an
	// eighth of the panorama's width: the lobe is broad at any roughness), another texture's mips, after levels in the
	// same memory
	std::vector<image::MipLevel> sheenLevels;
	size_t size = 0; // in bytes, of all levels
	std::array<std::array<float, 4>, 9> irradiance{};
	std::vector<DominantLight> dominantLights;
};

// the radiance a white Lambertian surface of normal n (unit length) reflects (as the shader evaluates it)
[[nodiscard]] glm::vec3 EvaluateIrradiance(const Environment& environment, const glm::vec3& n);

// the mean radiance of a panorama, or of a prefiltered level (as half floats), weighted by solid angle
[[nodiscard]] glm::dvec3 MeanRadiance(const Panorama& panorama);
[[nodiscard]] glm::dvec3 MeanRadiance(const image::MipLevel& level, std::span<const std::byte> data);

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE::environment
