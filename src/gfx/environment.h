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

// the default environment, where no file is loaded (and for tests): a sky over a ground, without a sun (the default
// directional light is one), as bright on average as the constant ambient light it replaced (0.3)
[[nodiscard]] Panorama ProceduralSky(uint32_t width = kSkyWidth);

// prefiltered levels: level i is the panorama filtered for roughness i / (kLevelCount - 1), half the size of the one
// before (level 0, roughness 0, is the panorama itself)
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
