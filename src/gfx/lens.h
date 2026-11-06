#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

// the lens (View > Camera > Lens, SPEEDO_LENS; the user's call, 2026-11-05: lens types whose artifacts arise from the
// lens rather than from sliders): a prescription, its glass surfaces from the front as a patent lists them (a radius, the
// distance to the next, the glass after it by its index and its Abbe number, its clear radius), one of them the aperture
// stop, with its blade count and whether its surfaces are coated. from it, by paraxial ray transfer matrices (height and
// angle through refractions and translations, Lee and Eisemann, "Practical Real-Time Lens-Flare Rendering", 2013, after
// Hullin et al. 2011), the cpu derives what the camera shows: the effective focal length; the vignetting over the field
// (the natural cos^4, and the mechanical one, the share of the entrance pupil whose rays pass every clear radius, the
// cat's eye, strongest wide open and off axis); the lateral chromatic aberration (the image's height per wavelength by the
// glasses' dispersion); and each ghost, a pair of reflecting surfaces, as an affine map of the entrance pupil onto the
// sensor with its color (Fresnel at both, per wavelength through the coating) and its share of the light. the pinhole
// (kPinhole) has no surfaces: no artifacts, the camera as it was
namespace gfx::lens
{

struct Surface
{
	float radius = 0.0F; // mm; 0: flat. positive: the center of curvature behind the surface (toward the sensor)
	float thickness = 0.0F; // mm to the next surface (the last: to the sensor, at the focus)
	float index = 1.0F; // the glass after the surface at the d line (587.6 nm); 1: air
	float abbe = 0.0F; // its Abbe number (the dispersion: n_F - n_C = (n_d - 1) / abbe); 0 for air
	float clearRadius = 0.0F; // mm: the surface's semi-diameter, past which rays are cut
};

// the wavelengths (nm) the red, green and blue channels stand for
inline constexpr std::array kWavelengths{650.0F, 550.0F, 450.0F};

struct Prescription
{
	std::string name;
	std::string key; // what SPEEDO_LENS takes
	std::vector<Surface> surfaces;
	size_t stopSurface = 0; // the aperture stop's index among the surfaces (a flat surface in air at its place)
	float focalLength = 50.0F; // mm, nominal: the prescription is scaled to give it (see Scale)
	float widestAperture = 1.4F; // the f-number wide open
	float narrowestAperture = 22.0F;
	uint32_t bladeCount = 7;
	bool coated = true; // a quarter wave of magnesium fluoride on every glass-air surface, tuned to 550 nm
	float distortion = 0.0F; // the third order coefficient in the normalized image's radius (negative: barrel)
	float squeeze = 1.0F; // anamorphic: the horizontal squeeze (2 for a 2x anamorphic), 1 spherical
	float transmittance = 0.98F; // what the whole lens lets through on axis
	// an anamorphic lens's cylinders: the front group's surfaces with power across (radii, mm, 0 flat; a Galilean pair of
	// focal lengths a squeeze apart, so afocal), in glass of cylinderIndex, whose reflections streak a light across (see
	// Streaks)
	std::vector<float> cylinderRadii;
	float cylinderIndex = 1.6F;
	// the wavelength (nm) the cylinders' quarter wave coating is tuned to: the red, so that the blue reflects most (the
	// blue streak anamorphic lenses are known for; tuned to the green, as the spherical surfaces are, it was magenta)
	float cylinderCoating = 650.0F;
};

// the lenses offered (View > Camera > Lens), the pinhole first: no surfaces, a focal length of its own on any sensor
[[nodiscard]] const std::vector<Prescription>& Lenses();
[[nodiscard]] const Prescription& Pinhole();
[[nodiscard]] std::optional<size_t> LensOf(std::string_view key);
[[nodiscard]] inline bool IsPinhole(const Prescription& lens) noexcept { return lens.surfaces.empty(); }

// the index of a glass at a wavelength (nm) from its index at the d line and its Abbe number (a dispersion linear in the
// inverse square of the wavelength through the F and C lines)
[[nodiscard]] float IndexAt(float index, float abbe, float wavelength) noexcept;

// a paraxial ray transfer matrix: (height, angle) after = M * (height, angle) before
using Matrix = glm::mat2;

// the matrices of the system at a wavelength: from the entrance (the first surface's plane, in air) to the sensor, and
// to the aperture stop's plane. the effective focal length is -1 / c of the entrance to sensor matrix's... see Trace
struct Traced
{
	Matrix toSensor{1.0F};
	Matrix toStop{1.0F};
	float focalLength = 0.0F; // mm, effective
	float backFocalDistance = 0.0F; // mm, from the last surface to the sensor, where the prescription puts it
	float entrancePupilRadius = 0.0F; // mm: the aperture stop as seen through the elements before it
};

// traces the prescription at a wavelength with the aperture stop open to stopRadius (mm; 0: wide open, the stop
// surface's clear radius)
[[nodiscard]] Traced Trace(const Prescription& lens, float wavelength, float stopRadius = 0.0F);

// scales the prescription (radii, thicknesses, clear radii) so that its effective focal length at the d line is its
// nominal focalLength
void Scale(Prescription& lens);

// the aperture stop's radius for an f-number (the entrance pupil's diameter is the focal length over it)
[[nodiscard]] float StopRadiusFor(const Prescription& lens, float fNumber);

// the aperture: the blades' polygon (its apothem the stop's radius at the f-number) cut by the barrel's round opening
// (the stop's radius wide open, the stop surface's clear radius). its edges' straight length (mm), which the starburst
// diffracts from: none wide open, where the blades are retracted behind the opening (a round aperture), the chords the
// opening leaves of them a little stopped down, the polygon's whole edges past that
[[nodiscard]] float OpenRadius(const Prescription& lens) noexcept;
[[nodiscard]] float StraightEdgeLength(const Prescription& lens, float stopRadius) noexcept;

// what the lens shows at an f-number (see the header's comment): the tables the final pass and the flares take
struct Appearance
{
	// the vignetting over the field: the light a point at the field angle (0 to the field's edge, by the sensor's half
	// diagonal over the focal length) gets as a share of the axis's, kVignettingSamples of them from the axis out
	static constexpr size_t kVignettingSamples = 32;
	std::array<float, kVignettingSamples> vignetting{};
	float fieldAngle = 0.0F; // radians: the half diagonal's, the last sample's
	// the lateral chromatic aberration: the red and the blue images' heights over the green's
	float redScale = 1.0F;
	float blueScale = 1.0F;
	float focalLength = 0.0F; // mm, effective, at the d line
	// and at the red's and the blue's wavelengths (the longitudinal chromatic aberration: their images focus before or
	// after the green's)
	float focalLengthRed = 0.0F;
	float focalLengthBlue = 0.0F;
	// the cat's eye: at each of the vignetting's field angles, how far (in radii) the entrance pupil's disk and the one
	// that clips it stand apart for their overlap to pass the mechanical share of the light, so that the bokeh of a
	// point of light there is the two disks' lens shape, pointing to the center (0 on the axis, a disk)
	std::array<float, kVignettingSamples> catEye{};
	// the bokeh's radial profile (see Bokeh and TraceRay: the lens's spherical aberration spreads a point's rays over
	// its circle of confusion unevenly, a bright rim on one side of the focus and a soft edge on the other), the light
	// at kBokehSamples radii from a circle's center to its edge as a share of a uniform disk's, for a point nearer than
	// the focus and one farther, at the reference circle (bokehReference: the circle's radius, mm, where the aberration
	// is a quarter of the defocus; larger circles tend to uniform as the defocus outgrows the aberration), and the
	// aberration itself (mm per mm cubed of the entrance height, negative undercorrected)
	static constexpr size_t kBokehSamples = 32;
	std::array<float, kBokehSamples> bokehNear{};
	std::array<float, kBokehSamples> bokehFar{};
	float bokehReference = 0.0F;
	float sphericalAberration = 0.0F;
	float entrancePupilRadius = 0.0F; // mm
	float stopRadius = 0.0F; // mm, at the f-number (the blades' polygon's apothem)
	float transmittance = 1.0F;
	// the share of the light the lens veils over the whole image (its glass's and coatings' scatter, beyond the ghosts
	// the surfaces reflect: a veiling glare index of a good coated lens is about a percent, an uncoated one's a few)
	float glare = 0.0F;
};

[[nodiscard]] Appearance Appear(const Prescription& lens, float fNumber, float sensorHalfDiagonal);

// a meridional ray traced exactly through the spherical surfaces (Snell's law at each, the stop open to stopRadius, 0
// wide open) from a height (mm) and an angle (radians, up) at the entrance, to the sensor: its height there, or none
// where it is cut (a clear radius, the stop) or reflected in full (what the paraxial matrices leave out: the
// aberrations, see Appearance::sphericalAberration)
[[nodiscard]] std::optional<float> TraceRay(const Prescription& lens, float wavelength, float height, float angle, float stopRadius = 0.0F, size_t* cutAt = nullptr);

// a ghost: a pair of reflecting surfaces, the ray reflected back at the second (later) and forward again at the first
// (earlier), as an affine map of the entrance pupil's (height, angle) onto the sensor, per wavelength
struct Ghost
{
	uint32_t first = 0; // the surfaces, the first nearer the front
	uint32_t second = 0;
	std::array<Matrix, 3> toSensor{}; // per wavelength (kWavelengths)
	std::array<Matrix, 3> toStop{}; // the ray's height at the aperture stop's plane, which clips it
	glm::vec3 reflectance{0.0F}; // the two surfaces' Fresnel reflectances' product, per wavelength
};

// every ghost of the lens (its glass-air surfaces' pairs; a cemented surface reflects too little), the stop open to an
// f-number
[[nodiscard]] std::vector<Ghost> Ghosts(const Prescription& lens, float fNumber);

// an anamorphic lens's streak (the user's ask, 2026-11-06, with a reference of a long blue streak through a light): a
// pair of its cylinders' surfaces reflects a light back and forward again, and the cylinders have power across alone, so
// the ghost is in focus up and down (on the light's row) and defocused across into a line: the vergence the two
// reflections add, twice the difference of the surfaces' curvatures, over the entrance pupil's diameter times the focal
// length on the sensor (the defocus of a thin lens: the image displaced f^2 times the vergence, its blur the pupil's
// diameter over f times that). its light is the two reflectances' product (per channel, through the coating: blue and red
// reflect most), spread evenly along the line
struct Streak
{
	glm::vec3 reflectance{0.0F};
	float halfLength = 0.0F; // mm on the sensor, across
};

// every pair of the cylinders' surfaces whose reflections defocus (two flat surfaces' ghost is the light itself), at an
// f-number; none for a spherical lens
[[nodiscard]] std::vector<Streak> Streaks(const Prescription& lens, float fNumber);

// the reflectance of a surface between two indices at a wavelength (nm): Fresnel at normal incidence, through a quarter
// wave coating of magnesium fluoride tuned to a wavelength (550 nm, the green, by default) where coated (a single layer's
// interference; a glass to glass surface, cemented, is never coated)
[[nodiscard]] float Reflectance(float indexBefore, float indexAfter, float wavelength, bool coated, float tuned = 550.0F) noexcept;

} // namespace gfx::lens
