#pragma once

#include <gfx/imageimport.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

// the environment the scene is lit by (image based lighting): a panorama of the radiance around it, prefiltered for the
// glTF brdf (see Shade in the shaders). cpu only, as the importers.
namespace gfx::environment
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

// the procedural sky's sun: where it is (towards it), and the irradiance it gives a surface facing it (lux, as the
// directional light it replaced: 2.2, the diffuse light of a white surface facing it (0.7) times pi)
constexpr std::array<float, 3> kSkySunDirection{0.2592F, 0.8639F, 0.4319F}; // normalize(0.3, 1, 0.5)
constexpr float kSkySunIrradiance = 2.2F;
constexpr float kSkySunRadius = 0.025F; // radians (1.4 degrees, larger than the real sun's)

// the procedural sky, as ProceduralSky draws it and the shader evaluates it where it is seen sharply (the backdrop, mirror
// reflections; see EnvironmentData): its colors, scaled so that the sky is as bright on average as the ambient light it
// replaced (0.3), and its sun disk, of a radiance giving kSkySunIrradiance
struct SkyParameters
{
	std::array<float, 3> zenith{};
	std::array<float, 3> horizon{};
	std::array<float, 3> ground{};
	std::array<float, 3> sunDirection{}; // towards it
	float sunCosRadius = 1.0F;
	float sunRadiance = 0.0F;
};
[[nodiscard]] const SkyParameters& ProceduralSkyParameters();

// the procedural sky's radiance (without its sun) in a direction (unit length), as the shader's SkyRadiance
[[nodiscard]] std::array<float, 3> SkyRadiance(const SkyParameters& sky, const std::array<float, 3>& direction);

// the default environment, where no file is loaded (and for tests): a sky over a ground, as bright on average as the
// constant ambient light it replaced (0.3), and a sun (see SkyParameters). a small panorama: what lights the scene is
// blurry (the prefiltered levels, the irradiance; the sun is a directional light), and the shader draws what is seen
// sharply itself
[[nodiscard]] Panorama ProceduralSky(uint32_t width = kSkyWidth);

// a panorama's dominant light: a strong compact light source (the sun, a lamp, a window), which lights the scene
// as a directional light (with shadows) instead of as part of the environment. direction: towards it (in the
// panorama's space), irradiance: what it gives a surface facing it, by color (radiance times solid angle, so in lux for
// a panorama in nits). none (zero irradiance) for a panorama without one (overcast, interiors lit evenly)
struct DominantLight
{
	std::array<float, 3> direction{0.0F, 1.0F, 0.0F};
	std::array<float, 3> irradiance{};

	[[nodiscard]] explicit operator bool() const noexcept { return irradiance[0] > 0.0F || irradiance[1] > 0.0F || irradiance[2] > 0.0F; }
};

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

// a panorama prefiltered for the shader: the specular levels, as R16G16B16A16 half floats, which are the mip levels of
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

// prefilters a panorama: allocate is called once with the size of the levels, which are written to the memory it
// returns, level 0 first. returns an error message if cancelled() returns true.
[[nodiscard]] std::expected<Environment, std::string> Prefilter(
	const Panorama& panorama,
	const std::function<std::byte*(size_t size)>& allocate,
	const std::function<bool()>& cancelled = {});

// the radiance a white Lambertian surface of normal n (unit length) reflects (as the shader evaluates it)
[[nodiscard]] std::array<float, 3> EvaluateIrradiance(const Environment& environment, const std::array<float, 3>& n);

// the mean radiance of a panorama, or of a prefiltered level (as half floats), weighted by solid angle
[[nodiscard]] std::array<double, 3> MeanRadiance(const Panorama& panorama);
[[nodiscard]] std::array<double, 3> MeanRadiance(const image::MipLevel& level, std::span<const std::byte> data);

} // namespace gfx::environment
