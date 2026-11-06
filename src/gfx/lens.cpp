#include <gfx/lens.h>

#include <core/assert.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gfx::lens
{

namespace
{

// a matrix by its entries: (height, angle) after = (a height + b angle, c height + d angle)
[[nodiscard]] Matrix Make(float a, float b, float c, float d) noexcept
{
	Matrix m(1.0F);
	m[0][0] = a;
	m[1][0] = b;
	m[0][1] = c;
	m[1][1] = d;
	return m;
}

[[nodiscard]] float A(const Matrix& m) noexcept { return m[0][0]; }
[[nodiscard]] float B(const Matrix& m) noexcept { return m[1][0]; }
[[nodiscard]] float C(const Matrix& m) noexcept { return m[0][1]; }
[[nodiscard]] float D(const Matrix& m) noexcept { return m[1][1]; }

// a translation by a distance (mm) in one medium
[[nodiscard]] Matrix Translate(float distance) noexcept { return Make(1.0F, distance, 0.0F, 1.0F); }

// a refraction at a spherical surface of a radius (0: flat) from one index into another
[[nodiscard]] Matrix Refract(float indexBefore, float indexAfter, float radius) noexcept
{
	float power = radius != 0.0F ? (indexBefore - indexAfter) / (radius * indexAfter) : 0.0F;
	return Make(1.0F, 0.0F, power, indexBefore / indexAfter);
}

// a reflection at a spherical surface of a radius, the ray going on the way it came (its angle then measured along that
// way: the surfaces it meets again have their radii negated, see GhostPath)
[[nodiscard]] Matrix Reflect(float radius) noexcept { return Make(1.0F, 0.0F, radius != 0.0F ? 2.0F / radius : 0.0F, 1.0F); }

// the index before a surface: the glass after the previous one, air before the first
[[nodiscard]] float IndexBefore(const Prescription& lens, size_t surface, float wavelength) noexcept
{
	if (surface == 0)
		return 1.0F;
	const auto& previous = lens.surfaces[surface - 1];
	return IndexAt(previous.index, previous.abbe, wavelength);
}

[[nodiscard]] float IndexAfter(const Prescription& lens, size_t surface, float wavelength) noexcept
{
	const auto& it = lens.surfaces[surface];
	return IndexAt(it.index, it.abbe, wavelength);
}

// the matrix from the entrance (just before the first surface) to just after surface last, then through the
// translation after it where throughLast
[[nodiscard]] Matrix Forward(const Prescription& lens, float wavelength, size_t last, bool throughLast)
{
	Matrix m(1.0F);
	for (size_t surface = 0; surface <= last; surface++)
	{
		m = Refract(IndexBefore(lens, surface, wavelength), IndexAfter(lens, surface, wavelength), lens.surfaces[surface].radius) * m;
		if (surface < last || throughLast)
			m = Translate(lens.surfaces[surface].thickness) * m;
	}
	return m;
}

// a ghost's path at a wavelength (see Ghost): forward to the second surface, reflected there, back to the first,
// reflected there, forward to the sensor; and where it crosses the aperture stop's plane first
void GhostPath(const Prescription& lens, uint32_t first, uint32_t second, float wavelength, Matrix& toSensor, Matrix& toStop)
{
	// forward to the second surface's vertex (through the stop's plane on the way, if the stop is before it)
	Matrix m = Forward(lens, wavelength, second, false);
	toStop = lens.stopSurface <= second ? Forward(lens, wavelength, lens.stopSurface, false) : Matrix(0.0F);
	// reflected at the second, in the medium before it
	m = Reflect(lens.surfaces[second].radius) * m;
	// back to the first: the translations between, and the surfaces between refracted the other way, their radii
	// negated
	for (size_t surface = second; surface > first; surface--)
	{
		m = Translate(lens.surfaces[surface - 1].thickness) * m;
		if (surface - 1 > first)
			m = Refract(IndexAfter(lens, surface - 1, wavelength), IndexBefore(lens, surface - 1, wavelength), -lens.surfaces[surface - 1].radius) * m;
	}
	// reflected at the first, met from behind
	m = Reflect(-lens.surfaces[first].radius) * m;
	// forward again from the first to the sensor
	for (size_t surface = first; surface < lens.surfaces.size(); surface++)
	{
		m = Translate(lens.surfaces[surface].thickness) * m;
		if (surface + 1 < lens.surfaces.size())
			m = Refract(IndexBefore(lens, surface + 1, wavelength), IndexAfter(lens, surface + 1, wavelength), lens.surfaces[surface + 1].radius) * m;
		if (lens.stopSurface == surface + 1 && lens.stopSurface > second)
			toStop = m; // the stop past both: the ghost crosses it once, on the way out
	}
	toSensor = m;
}

// whether a surface reflects: between two media (not the stop's plane in air)
[[nodiscard]] bool Reflects(const Prescription& lens, size_t surface) noexcept
{
	return std::abs(IndexBefore(lens, surface, 550.0F) - IndexAfter(lens, surface, 550.0F)) > 1e-4F;
}

[[nodiscard]] bool Cemented(const Prescription& lens, size_t surface) noexcept
{
	return IndexBefore(lens, surface, 550.0F) > 1.0001F && IndexAfter(lens, surface, 550.0F) > 1.0001F;
}

// an element of a lens built from its elements: a thin lens's power as a thick element of a glass, its front and back
// radii as a share of a sphere's (shape: 1 a plano-convex front, 0 equiconvex, -1 a plano-convex back; a negative power
// swaps the curvatures' signs), with its clear radius
struct Element
{
	float focalLength; // mm (negative: diverging)
	float thickness; // mm at the axis
	float index;
	float abbe;
	float clearRadius;
	float shape = 0.0F;
	float spacing = 1.0F; // mm of air after it
};

[[nodiscard]] Prescription FromElements(std::string name, std::string key, float focalLength, std::span<const Element> elements, size_t stopAfter, float stopSpacing)
{
	Prescription lens{.name = std::move(name), .key = std::move(key), .focalLength = focalLength};
	for (size_t elementIt = 0; elementIt < elements.size(); elementIt++)
	{
		const auto& element = elements[elementIt];
		// a thin lens of two surfaces: 1/f = (n - 1) (1 / r1 - 1 / r2), split by the shape
		float power = 1.0F / element.focalLength;
		float frontShare = 0.5F * (1.0F + element.shape);
		float backShare = 1.0F - frontShare;
		float r1 = frontShare > 1e-3F ? (element.index - 1.0F) / (power * frontShare) : 0.0F;
		float r2 = backShare > 1e-3F ? -(element.index - 1.0F) / (power * backShare) : 0.0F;
		lens.surfaces.push_back(Surface{.radius = r1, .thickness = element.thickness, .index = element.index, .abbe = element.abbe, .clearRadius = element.clearRadius});
		lens.surfaces.push_back(Surface{.radius = r2, .thickness = element.spacing, .index = 1.0F, .abbe = 0.0F, .clearRadius = element.clearRadius});
		if (elementIt + 1 == stopAfter)
		{
			lens.surfaces.back().thickness = stopSpacing;
			lens.stopSurface = lens.surfaces.size();
			lens.surfaces.push_back(Surface{.radius = 0.0F, .thickness = element.spacing, .index = 1.0F, .abbe = 0.0F, .clearRadius = element.clearRadius * 0.8F});
		}
	}
	return lens;
}

// the chief ray's height on the sensor (mm) at a field angle (radians) and a wavelength: through the stop's center,
// found by the matrix to the stop
[[nodiscard]] float ChiefRayHeight(const Prescription& lens, float wavelength, float angle, float stopRadius)
{
	auto traced = Trace(lens, wavelength, stopRadius);
	float chief = A(traced.toStop) != 0.0F ? -B(traced.toStop) * angle / A(traced.toStop) : 0.0F;
	return A(traced.toSensor) * chief + B(traced.toSensor) * angle;
}

// picks the Abbe number of the glass after a surface (its dispersion, between a flint's 20 and a crown's 95) that
// cancels the lateral chromatic aberration at the field's edge (the red and the blue chief rays' heights equal), by
// bisection where the two ends' differences have opposite signs, else the end with the smaller
void Achromatize(Prescription& lens, size_t surface)
{
	ENSURE(surface < lens.surfaces.size() && lens.surfaces[surface].index > 1.0F);
	float angle = 0.3F;
	auto difference = [&lens, surface, angle](float abbe)
	{
		lens.surfaces[surface].abbe = abbe;
		return ChiefRayHeight(lens, kWavelengths[0], angle, 0.0F) - ChiefRayHeight(lens, kWavelengths[2], angle, 0.0F);
	};
	float low = 20.0F;
	float high = 95.0F;
	float atLow = difference(low);
	float atHigh = difference(high);
	if (atLow * atHigh > 0.0F)
	{
		lens.surfaces[surface].abbe = std::abs(atLow) < std::abs(atHigh) ? low : high;
		return;
	}
	for (int step = 0; step < 40; step++)
	{
		float middle = 0.5F * (low + high);
		float atMiddle = difference(middle);
		if ((atMiddle < 0.0F) == (atLow < 0.0F))
		{
			low = middle;
			atLow = atMiddle;
		}
		else
			high = middle;
	}
	lens.surfaces[surface].abbe = 0.5F * (low + high);
}

// the lenses: a real double Gauss (Kolb, Mitchell and Hanrahan 1995, Table 1, "A Realistic Camera Model for Computer
// Graphics", which took it from Smith's Modern Lens Design; its stop at the flat surface in the middle), and designs
// built from elements with the topology of their kind
[[nodiscard]] std::vector<Prescription> MakeLenses()
{
	std::vector<Prescription> lenses;
	lenses.push_back(Prescription{.name = "Pinhole", .key = "pinhole", .focalLength = 0.0F, .widestAperture = 0.7F, .narrowestAperture = 64.0F, .bladeCount = 0, .transmittance = 1.0F});

	{
		Prescription gauss{.name = "50 mm f/1.8 double Gauss", .key = "gauss-50", .focalLength = 50.0F, .widestAperture = 1.8F, .bladeCount = 8, .coated = true, .distortion = 0.004F};
		gauss.surfaces = {
			{.radius = 58.950F, .thickness = 7.520F, .index = 1.670F, .abbe = 47.1F, .clearRadius = 25.2F},
			{.radius = 169.660F, .thickness = 0.240F, .index = 1.0F, .abbe = 0.0F, .clearRadius = 25.2F},
			{.radius = 38.550F, .thickness = 8.050F, .index = 1.670F, .abbe = 47.1F, .clearRadius = 23.0F},
			{.radius = 81.540F, .thickness = 6.550F, .index = 1.699F, .abbe = 30.1F, .clearRadius = 23.0F},
			{.radius = 25.500F, .thickness = 11.410F, .index = 1.0F, .abbe = 0.0F, .clearRadius = 18.0F},
			{.radius = 0.0F, .thickness = 9.000F, .index = 1.0F, .abbe = 0.0F, .clearRadius = 18.0F},
			{.radius = -28.990F, .thickness = 2.360F, .index = 1.603F, .abbe = 38.0F, .clearRadius = 18.0F},
			{.radius = 81.540F, .thickness = 12.130F, .index = 1.658F, .abbe = 57.3F, .clearRadius = 21.0F},
			{.radius = -40.770F, .thickness = 0.380F, .index = 1.0F, .abbe = 0.0F, .clearRadius = 21.0F},
			{.radius = 874.130F, .thickness = 6.440F, .index = 1.717F, .abbe = 48.0F, .clearRadius = 23.0F},
			{.radius = -79.460F, .thickness = 72.228F, .index = 1.0F, .abbe = 0.0F, .clearRadius = 23.0F},
		};
		gauss.stopSurface = 5;
		lenses.push_back(std::move(gauss));
	}
	{
		// a retrofocus wide angle: a diverging front group (-70 and -130 mm, -45 together) 35 mm before a converging rear
		// one (50, -60 and 40 mm, 35 together, the stop in it), which puts the focus 63 mm behind the rear group, farther
		// than the 35.5 mm focal length (scaled to 28); a big front element, as wide angles have, and barrel distortion
		const std::array elements{
			Element{.focalLength = -70.0F, .thickness = 3.0F, .index = 1.62F, .abbe = 60.0F, .clearRadius = 30.0F, .shape = 1.0F, .spacing = 10.0F},
			Element{.focalLength = -130.0F, .thickness = 2.5F, .index = 1.72F, .abbe = 50.0F, .clearRadius = 20.0F, .shape = 0.6F, .spacing = 20.0F},
			Element{.focalLength = 50.0F, .thickness = 6.0F, .index = 1.62F, .abbe = 60.0F, .clearRadius = 14.0F, .shape = 0.2F, .spacing = 3.0F},
			Element{.focalLength = -60.0F, .thickness = 2.0F, .index = 1.75F, .abbe = 28.0F, .clearRadius = 12.0F, .shape = -0.3F, .spacing = 3.0F},
			Element{.focalLength = 40.0F, .thickness = 6.0F, .index = 1.62F, .abbe = 60.0F, .clearRadius = 14.0F, .shape = -0.4F, .spacing = 40.0F},
		};
		auto wide = FromElements("28 mm f/2.8 wide angle", "wide-28", 28.0F, elements, 3, 2.5F);
		wide.widestAperture = 2.8F;
		wide.bladeCount = 7;
		wide.distortion = -0.06F;
		lenses.push_back(std::move(wide));
	}
	{
		// a telephoto portrait lens: a converging front group (95 and 130 mm, 55 together) 40 mm before a diverging rear
		// element (-35 mm), which puts the focus 26 mm behind it, shorter than the 96 mm focal length (scaled to 85); the
		// stop just behind the front group; a touch of pincushion
		const std::array elements{
			Element{.focalLength = 95.0F, .thickness = 7.0F, .index = 1.62F, .abbe = 60.0F, .clearRadius = 26.0F, .shape = 0.5F, .spacing = 1.0F},
			Element{.focalLength = 130.0F, .thickness = 6.0F, .index = 1.65F, .abbe = 55.0F, .clearRadius = 25.0F, .shape = 0.6F, .spacing = 32.0F},
			Element{.focalLength = -35.0F, .thickness = 2.5F, .index = 1.76F, .abbe = 27.0F, .clearRadius = 15.0F, .shape = -0.3F, .spacing = 26.0F},
		};
		auto portrait = FromElements("85 mm f/1.8 portrait", "portrait-85", 85.0F, elements, 2, 8.0F);
		portrait.widestAperture = 1.8F;
		portrait.bladeCount = 9;
		portrait.distortion = 0.012F;
		lenses.push_back(std::move(portrait));
	}
	{
		// a Tessar of the 1950s: four elements in three groups (a converging front, a diverging middle, the stop, a
		// cemented doublet), uncoated, so every surface reflects 4 to 7% and the ghosts and the veiling glare are strong;
		// 6 straight blades
		const std::array elements{
			Element{.focalLength = 45.0F, .thickness = 5.0F, .index = 1.61F, .abbe = 58.0F, .clearRadius = 14.0F, .shape = 0.7F, .spacing = 3.5F},
			Element{.focalLength = -32.0F, .thickness = 2.0F, .index = 1.62F, .abbe = 36.0F, .clearRadius = 11.0F, .shape = -0.2F, .spacing = 4.0F},
			Element{.focalLength = -80.0F, .thickness = 2.0F, .index = 1.57F, .abbe = 41.0F, .clearRadius = 12.0F, .shape = 0.9F, .spacing = 0.0F},
			Element{.focalLength = 28.0F, .thickness = 6.0F, .index = 1.65F, .abbe = 55.0F, .clearRadius = 12.0F, .shape = -0.6F, .spacing = 38.0F},
		};
		auto vintage = FromElements("50 mm f/2.8 vintage (uncoated)", "vintage-50", 50.0F, elements, 2, 3.0F);
		vintage.widestAperture = 2.8F;
		vintage.bladeCount = 6;
		vintage.coated = false;
		vintage.distortion = -0.01F;
		vintage.transmittance = 0.85F;
		lenses.push_back(std::move(vintage));
	}
	{
		// a 2x anamorphic: the double Gauss behind a cylindrical front group, which squeezes the width (the ghosts and
		// the flares with it: streaks across) and leaves the height
		auto anamorphic = lenses[1];
		anamorphic.name = "50 mm 2x anamorphic";
		anamorphic.key = "anamorphic-50";
		anamorphic.squeeze = 2.0F;
		// a Galilean pair of plano cylinders, -100 mm and 200 mm (n - 1 = 0.6), the squeeze apart
		anamorphic.cylinderRadii = {0.0F, -60.0F, 120.0F, 0.0F};
		anamorphic.bladeCount = 10;
		anamorphic.distortion = -0.02F;
		anamorphic.transmittance = 0.9F;
		lenses.push_back(anamorphic);
		// a cinema set's other primes (the user's ask, 2026-11-06): a 40 mm 2x, whose horizontal field is a 20 mm's, and
		// a 75 mm 2x, the Gauss scaled to them (see Scale) behind the same cylinders; and a 1.33x, which squeezes a 2.39:1
		// frame onto a 16:9 sensor, a modern one's lighter streak and rounder bokeh
		auto wideAnamorphic = anamorphic;
		wideAnamorphic.name = "40 mm 2x anamorphic";
		wideAnamorphic.key = "anamorphic-40";
		wideAnamorphic.focalLength = 40.0F;
		wideAnamorphic.distortion = -0.04F;
		wideAnamorphic.widestAperture = 2.2F;
		lenses.push_back(std::move(wideAnamorphic));
		auto longAnamorphic = anamorphic;
		longAnamorphic.name = "75 mm 2x anamorphic";
		longAnamorphic.key = "anamorphic-75";
		longAnamorphic.focalLength = 75.0F;
		longAnamorphic.distortion = -0.01F;
		longAnamorphic.widestAperture = 2.0F;
		lenses.push_back(std::move(longAnamorphic));
		auto lightAnamorphic = anamorphic;
		lightAnamorphic.name = "50 mm 1.33x anamorphic";
		lightAnamorphic.key = "anamorphic-50-133";
		lightAnamorphic.squeeze = 1.33F;
		lightAnamorphic.cylinderRadii = {0.0F, -90.0F, 120.0F, 0.0F}; // -150 mm and 200 mm
		lightAnamorphic.bladeCount = 11;
		lightAnamorphic.distortion = -0.015F;
		lightAnamorphic.transmittance = 0.93F;
		lenses.push_back(std::move(lightAnamorphic));
	}
	// the wide angles a first person game is played through (the user's ask, 2026-11-06: on a 16:9 window of a full
	// frame sensor, 20 mm sees 84 degrees across, 16 mm 97 and 12 mm 113, the fields games call 90 to 110), each a
	// retrofocus as the 28: a diverging front group the wider the shorter, the stop in the converging rear group, the
	// focus well behind it, big front elements. rectilinear, as a game's projection is: a few percent of barrel
	// distortion at the corner, as modern ones have (at 15% the 12 mm looked a fisheye's)
	auto retrofocus = [&lenses](const char* name, const char* key, float focalLength, float frontFocal, float frontRadius, float widest, uint32_t blades, float distortion)
	{
		const std::array elements{
			Element{.focalLength = frontFocal, .thickness = 3.0F, .index = 1.62F, .abbe = 60.0F, .clearRadius = frontRadius, .shape = 1.0F, .spacing = 10.0F},
			Element{.focalLength = 1.8F * frontFocal, .thickness = 2.5F, .index = 1.72F, .abbe = 50.0F, .clearRadius = 0.7F * frontRadius, .shape = 0.6F, .spacing = 20.0F},
			Element{.focalLength = 50.0F, .thickness = 6.0F, .index = 1.62F, .abbe = 60.0F, .clearRadius = 14.0F, .shape = 0.2F, .spacing = 3.0F},
			Element{.focalLength = -60.0F, .thickness = 2.0F, .index = 1.75F, .abbe = 28.0F, .clearRadius = 12.0F, .shape = -0.3F, .spacing = 3.0F},
			Element{.focalLength = 40.0F, .thickness = 6.0F, .index = 1.62F, .abbe = 60.0F, .clearRadius = 14.0F, .shape = -0.4F, .spacing = 40.0F},
		};
		auto lens = FromElements(name, key, focalLength, elements, 3, 2.5F);
		lens.widestAperture = widest;
		lens.bladeCount = blades;
		lens.distortion = distortion;
		lenses.push_back(std::move(lens));
	};
	retrofocus("20 mm f/1.8 wide angle", "wide-20", 20.0F, -55.0F, 34.0F, 1.8F, 7, -0.025F);
	retrofocus("16 mm f/2.8 ultra wide", "ultrawide-16", 16.0F, -45.0F, 38.0F, 2.8F, 7, -0.03F);
	retrofocus("12 mm f/2.8 ultra wide", "ultrawide-12", 12.0F, -38.0F, 42.0F, 2.8F, 9, -0.035F);
	{
		// a fast 35: a double Gauss's symmetry, two converging elements, a diverging one, the stop, a diverging one and two
		// converging, fast and with a little barrel. the elements stand apart in air (an element's convex back and the
		// next one's concave front overlap off the axis at no spacing: the exact trace, TraceRay, is cut by it where the
		// paraxial one isn't), and the front group is weak, as a Gauss's is, so that the marginal ray wide open stays
		// shallow through the middle (a strong front group bent it 50 degrees)
		const std::array elements{
			Element{.focalLength = 90.0F, .thickness = 6.0F, .index = 1.69F, .abbe = 50.0F, .clearRadius = 22.0F, .shape = 0.8F, .spacing = 0.5F},
			Element{.focalLength = 75.0F, .thickness = 7.0F, .index = 1.67F, .abbe = 47.0F, .clearRadius = 20.0F, .shape = 0.5F, .spacing = 3.0F},
			Element{.focalLength = -45.0F, .thickness = 2.5F, .index = 1.70F, .abbe = 30.0F, .clearRadius = 17.0F, .shape = -0.6F, .spacing = 8.0F},
			Element{.focalLength = -48.0F, .thickness = 2.5F, .index = 1.60F, .abbe = 38.0F, .clearRadius = 17.0F, .shape = 0.6F, .spacing = 1.5F},
			Element{.focalLength = 55.0F, .thickness = 8.0F, .index = 1.66F, .abbe = 57.0F, .clearRadius = 19.0F, .shape = -0.5F, .spacing = 0.5F},
			Element{.focalLength = 90.0F, .thickness = 5.0F, .index = 1.72F, .abbe = 48.0F, .clearRadius = 21.0F, .shape = -0.8F, .spacing = 36.0F},
		};
		auto fast = FromElements("35 mm f/1.4", "fast-35", 35.0F, elements, 3, 8.0F);
		fast.widestAperture = 1.4F;
		fast.bladeCount = 9;
		fast.distortion = -0.02F;
		lenses.push_back(std::move(fast));
	}
	{
		// a 135 telephoto: a converging front group (110 and 150 mm, 63 together) and a diverging rear (-40 mm) 30 mm
		// behind it, as the 85, longer; a touch of pincushion. the rear stands well before the front group's focus (at
		// 45 mm it stood at it, the design's focal length ran to 1350 mm and the scale left 4 mm of back focus)
		const std::array elements{
			Element{.focalLength = 110.0F, .thickness = 14.0F, .index = 1.62F, .abbe = 60.0F, .clearRadius = 42.0F, .shape = 0.2F, .spacing = 1.0F},
			Element{.focalLength = 150.0F, .thickness = 12.0F, .index = 1.65F, .abbe = 55.0F, .clearRadius = 40.0F, .shape = 0.3F, .spacing = 18.0F},
			Element{.focalLength = -55.0F, .thickness = 3.0F, .index = 1.76F, .abbe = 27.0F, .clearRadius = 26.0F, .shape = -0.3F, .spacing = 40.0F},
		};
		auto tele = FromElements("135 mm f/2 telephoto", "tele-135", 135.0F, elements, 2, 10.0F);
		tele.widestAperture = 2.0F;
		tele.bladeCount = 9;
		tele.distortion = 0.01F;
		lenses.push_back(std::move(tele));
	}

	for (auto& lens : lenses)
		if (!IsPinhole(lens))
			Scale(lens);
	// the designs built from elements achromatized by two glasses' dispersions each, the correctors, as a designer picks
	// glasses: each design's front element and a rear negative element, in turns (the Tessar keeps its fringes, a
	// vintage lens's; the anamorphic is the Gauss)
	for (int round = 0; round < 4; round++)
	{
		for (size_t lensIt = 0; lensIt < lenses.size(); lensIt++)
		{
			const auto& key = lenses[lensIt].key;
			if (key == "wide-28" || key == "wide-20" || key == "ultrawide-16" || key == "ultrawide-12")
			{
				Achromatize(lenses[lensIt], 0);
				Achromatize(lenses[lensIt], 2);
				Achromatize(lenses[lensIt], 4);
				Achromatize(lenses[lensIt], 7);
				Achromatize(lenses[lensIt], 9);
			}
			else if (key == "portrait-85" || key == "tele-135")
			{
				Achromatize(lenses[lensIt], 0);
				Achromatize(lenses[lensIt], 5);
			}
			else if (key == "fast-35")
			{
				// its rear negative element's front, past the stop
				Achromatize(lenses[lensIt], 0);
				Achromatize(lenses[lensIt], 7);
			}
		}
	}
	return lenses;
}

} // namespace

const std::vector<Prescription>& Lenses()
{
	static const std::vector<Prescription> kLenses = MakeLenses();
	return kLenses;
}

const Prescription& Pinhole()
{
	return Lenses().front();
}

std::optional<size_t> LensOf(std::string_view key)
{
	const auto& lenses = Lenses();
	for (size_t lensIt = 0; lensIt < lenses.size(); lensIt++)
		if (lenses[lensIt].key == key)
			return lensIt;
	return std::nullopt;
}

float IndexAt(float index, float abbe, float wavelength) noexcept
{
	if (abbe <= 0.0F || index <= 1.0F)
		return index;
	constexpr float kD = 587.6F;
	constexpr float kF = 486.1F;
	constexpr float kC = 656.3F;
	auto inverseSquare = [](float nm) { return 1.0F / (nm * nm); };
	float dispersion = (index - 1.0F) / abbe; // n_F - n_C
	return index + dispersion * (inverseSquare(wavelength) - inverseSquare(kD)) / (inverseSquare(kF) - inverseSquare(kC));
}

Traced Trace(const Prescription& lens, float wavelength, float stopRadius)
{
	Traced traced;
	if (IsPinhole(lens))
	{
		traced.focalLength = lens.focalLength;
		return traced;
	}
	ENSURE(lens.stopSurface < lens.surfaces.size());
	// to the last surface's vertex: the focal length and the back focal distance; the sensor where the prescription puts
	// it, its last thickness
	Matrix toVertex = Forward(lens, wavelength, lens.surfaces.size() - 1, false);
	traced.focalLength = C(toVertex) != 0.0F ? -1.0F / C(toVertex) : 0.0F;
	traced.backFocalDistance = C(toVertex) != 0.0F ? -A(toVertex) / C(toVertex) : 0.0F;
	traced.toSensor = Forward(lens, wavelength, lens.surfaces.size() - 1, true);
	traced.toStop = Forward(lens, wavelength, lens.stopSurface, false);
	// the entrance pupil: the stop as seen from the front, a parallel ray's height at the entrance that reaches the
	// stop's edge
	float radius = stopRadius > 0.0F ? stopRadius : lens.surfaces[lens.stopSurface].clearRadius;
	traced.entrancePupilRadius = A(traced.toStop) != 0.0F ? radius / A(traced.toStop) : radius;
	return traced;
}

void Scale(Prescription& lens)
{
	if (IsPinhole(lens))
		return;
	auto traced = Trace(lens, 587.6F);
	ENSUREF(traced.focalLength > 0.0F, "lens {}: no focus (an effective focal length of {})", lens.name, traced.focalLength);
	float factor = lens.focalLength / traced.focalLength;
	for (auto& surface : lens.surfaces)
	{
		surface.radius *= factor;
		surface.thickness *= factor;
		surface.clearRadius *= factor;
	}
	// the sensor at the focus of a point at infinity
	traced = Trace(lens, 587.6F);
	lens.surfaces.back().thickness = traced.backFocalDistance;
	// the stop opens to the widest aperture: its clear radius the entrance pupil's for it, seen through the elements before
	// it (the surfaces around it no smaller)
	float pupil = lens.focalLength / (2.0F * std::max(lens.widestAperture, 0.7F));
	float stopRadius = pupil * std::abs(A(traced.toStop));
	lens.surfaces[lens.stopSurface].clearRadius = stopRadius;
	for (size_t surface = 0; surface < lens.surfaces.size(); surface++)
		if (surface + 1 == lens.stopSurface || surface == lens.stopSurface + 1)
			lens.surfaces[surface].clearRadius = std::max(lens.surfaces[surface].clearRadius, stopRadius);
}

float StopRadiusFor(const Prescription& lens, float fNumber)
{
	if (IsPinhole(lens))
		return 0.0F;
	auto traced = Trace(lens, 587.6F);
	float pupil = lens.focalLength / (2.0F * std::max(fNumber, lens.widestAperture));
	return std::min(pupil * A(traced.toStop), lens.surfaces[lens.stopSurface].clearRadius);
}

float OpenRadius(const Prescription& lens) noexcept
{
	return IsPinhole(lens) ? 0.0F : lens.surfaces[lens.stopSurface].clearRadius;
}

float StraightEdgeLength(const Prescription& lens, float stopRadius) noexcept
{
	if (lens.bladeCount < 3 || IsPinhole(lens))
		return 0.0F;
	// the polygon's edge (2 a tan(pi / N) for the apothem a), or the chord the opening leaves of it where the polygon's
	// corners reach past the opening
	float open = OpenRadius(lens);
	float edge = 2.0F * stopRadius * std::tan(std::numbers::pi_v<float> / static_cast<float>(lens.bladeCount));
	float chord = 2.0F * std::sqrt(std::max(open * open - stopRadius * stopRadius, 0.0F));
	return std::min(edge, chord);
}

Appearance Appear(const Prescription& lens, float fNumber, float sensorHalfDiagonal)
{
	Appearance appearance;
	appearance.transmittance = lens.transmittance;
	if (IsPinhole(lens))
	{
		appearance.vignetting.fill(1.0F);
		appearance.focalLength = lens.focalLength;
		return appearance;
	}
	float stopRadius = StopRadiusFor(lens, fNumber);
	auto green = Trace(lens, kWavelengths[1], stopRadius);
	appearance.focalLength = green.focalLength;
	appearance.entrancePupilRadius = green.entrancePupilRadius;
	appearance.stopRadius = stopRadius;
	constexpr float kCoatedGlare = 0.005F;
	constexpr float kUncoatedGlare = 0.02F;
	appearance.glare = lens.coated ? kCoatedGlare : kUncoatedGlare;
	appearance.fieldAngle = std::atan(sensorHalfDiagonal / std::max(green.focalLength, 1e-3F));

	// the mechanical vignetting: of a grid of rays over the aperture stop's disk (height y along the field, x across it)
	// at the field angle, each traced back to the entrance by the matrix to the stop and forward from there, those that
	// pass every surface's clear radius (sagittal heights by the same matrices at no angle); the share over the axis's.
	// (sampled at the entrance's plane instead, as a disk of the pupil's radius, an off axis beam that passes the stop
	// doesn't enter where the disk is, and a wide angle's corners came out black)
	constexpr int kGrid = 24;
	auto passing = [&lens, &green, stopRadius](float angle)
	{
		float stopA = A(green.toStop);
		float stopB = B(green.toStop);
		if (std::abs(stopA) < 1e-6F)
			return 1.0F;
		int passed = 0;
		int total = 0;
		for (int yIt = 0; yIt < kGrid; yIt++)
			for (int xIt = 0; xIt < kGrid; xIt++)
			{
				float x = (static_cast<float>(xIt) + 0.5F) / kGrid * 2.0F - 1.0F;
				float y = (static_cast<float>(yIt) + 0.5F) / kGrid * 2.0F - 1.0F;
				if (x * x + y * y > 1.0F)
					continue;
				total++;
				// the ray's heights at the entrance, from its heights at the stop and its angle
				float entranceY = (y * stopRadius - stopB * angle) / stopA;
				float entranceX = x * stopRadius / stopA;
				// and at each surface, forward
				Matrix m(1.0F);
				bool passes = true;
				for (size_t surface = 0; surface < lens.surfaces.size() && passes; surface++)
				{
					float heightY = A(m) * entranceY + B(m) * angle;
					float heightX = A(m) * entranceX;
					float limit = surface == lens.stopSurface ? stopRadius * 1.001F : lens.surfaces[surface].clearRadius;
					passes = heightX * heightX + heightY * heightY <= limit * limit;
					m = Refract(IndexBefore(lens, surface, kWavelengths[1]), IndexAfter(lens, surface, kWavelengths[1]), lens.surfaces[surface].radius) * m;
					m = Translate(lens.surfaces[surface].thickness) * m;
				}
				passed += passes ? 1 : 0;
			}
		return total > 0 ? static_cast<float>(passed) / static_cast<float>(total) : 1.0F;
	};
	float onAxis = std::max(passing(0.0F), 1e-3F);
	// the cat's eye from the mechanical share: two disks of one radius a distance d apart (in radii) overlap by
	// (2 acos(d / 2) - (d / 2) sqrt(4 - d^2)) / pi of a disk, falling from 1 at d = 0 to 0 at 2; d by bisection
	auto overlap = [](float distance)
	{
		float half = 0.5F * distance;
		return (2.0F * std::acos(std::clamp(half, -1.0F, 1.0F)) - half * std::sqrt(std::max(4.0F - distance * distance, 0.0F))) / std::numbers::pi_v<float>;
	};
	// the natural vignetting: cos^4 of the angle the chief ray meets the sensor at, the exit pupil's solid angle as a point
	// of the image sees it (its distance, its tilt, the sensor's). the image side's angle is the field's times the
	// chief ray's slope after the lens over before it (its tangent, paraxially): a retrofocus wide angle's exit pupil
	// stands far before the sensor, so its corners get more light than the object side's cos^4 (which left a 12 mm's
	// corner, at 61 degrees, 0.056 of the axis's however it was built: real ones keep 0.15 to 0.3 wide open), and a
	// telephoto's near, so they get less
	float chiefSlope = 1.0F;
	if (std::abs(A(green.toStop)) > 1e-6F)
	{
		constexpr float kProbe = 0.01F;
		float chiefHeight = -B(green.toStop) * kProbe / A(green.toStop);
		chiefSlope = std::abs(C(green.toSensor) * chiefHeight + D(green.toSensor) * kProbe) / kProbe;
	}
	for (size_t sampleIt = 0; sampleIt < Appearance::kVignettingSamples; sampleIt++)
	{
		float angle = appearance.fieldAngle * static_cast<float>(sampleIt) / static_cast<float>(Appearance::kVignettingSamples - 1);
		float natural = std::pow(std::cos(std::atan(chiefSlope * std::tan(angle))), 4.0F);
		float mechanical = std::min(passing(angle) / onAxis, 1.0F);
		appearance.vignetting[sampleIt] = natural * mechanical;
		float low = 0.0F;
		float high = 2.0F;
		for (int stepIt = 0; stepIt < 24; stepIt++)
		{
			float middle = 0.5F * (low + high);
			(overlap(middle) > mechanical ? low : high) = middle;
		}
		appearance.catEye[sampleIt] = 0.5F * (low + high);
	}
	// the focal lengths at the red's and the blue's wavelengths
	appearance.focalLengthRed = Trace(lens, kWavelengths[0], stopRadius).focalLength;
	appearance.focalLengthBlue = Trace(lens, kWavelengths[2], stopRadius).focalLength;

	// the bokeh's profile from the spherical aberration (see Appearance::bokehNear): rays parallel to the axis across the
	// pupil, traced exactly to the sensor (the paraxial focus), their heights fitted as c1 y + c3 y^3 (c3 the aberration);
	// a defocused point's rays land at k y + c3 y^3, k the defocus (positive for a point nearer than the focus, whose
	// rays haven't met; negative farther), so the light over the circle is the rays' density there: uniform over the
	// pupil's area, counted by the radius they land at over the annulus's area
	{
		constexpr int kRays = 96;
		float pupil = std::max(green.entrancePupilRadius, 1e-3F);
		double sumY4 = 0.0;
		double sumY6 = 0.0;
		double sumY2 = 0.0;
		double sumHY = 0.0;
		double sumHY3 = 0.0;
		for (int rayIt = 0; rayIt < kRays; rayIt++)
		{
			float y = pupil * (static_cast<float>(rayIt) + 0.5F) / kRays;
			auto height = TraceRay(lens, kWavelengths[1], y, 0.0F, stopRadius);
			if (!height)
				continue;
			sumY2 += static_cast<double>(y) * y;
			sumY4 += std::pow(static_cast<double>(y), 4.0);
			sumY6 += std::pow(static_cast<double>(y), 6.0);
			sumHY += static_cast<double>(*height) * y;
			sumHY3 += static_cast<double>(*height) * std::pow(static_cast<double>(y), 3.0);
		}
		// the least squares of [y, y^3]: the normal equations' 2x2
		double determinant = sumY2 * sumY6 - sumY4 * sumY4;
		float aberration = 0.0F;
		if (std::abs(determinant) > 1e-12)
			aberration = static_cast<float>((sumY2 * sumHY3 - sumY4 * sumHY) / determinant);
		appearance.sphericalAberration = aberration;
		// the reference defocus: the aberration at the pupil's edge a quarter of it
		float reference = 4.0F * std::abs(aberration) * pupil * pupil;
		appearance.bokehReference = reference * pupil;
		auto profile = [aberration, pupil, reference](float sign, std::array<float, Appearance::kBokehSamples>& table)
		{
			table.fill(1.0F);
			if (reference <= 1e-9F)
				return;
			constexpr int kSamples = 4096;
			std::array<int, Appearance::kBokehSamples> counts{};
			float edge = 0.0F;
			std::array<float, kSamples> landings{};
			for (int sampleIt = 0; sampleIt < kSamples; sampleIt++)
			{
				float y = pupil * std::sqrt((static_cast<float>(sampleIt) + 0.5F) / kSamples);
				landings[static_cast<size_t>(sampleIt)] = std::abs(sign * reference * y + aberration * y * y * y);
				edge = std::max(edge, landings[static_cast<size_t>(sampleIt)]);
			}
			for (float landing : landings)
				counts[std::min<size_t>(static_cast<size_t>(landing / std::max(edge, 1e-9F) * Appearance::kBokehSamples), Appearance::kBokehSamples - 1)]++;
			for (size_t binIt = 0; binIt < Appearance::kBokehSamples; binIt++)
			{
				float inner = static_cast<float>(binIt) / Appearance::kBokehSamples;
				float outer = static_cast<float>(binIt + 1) / Appearance::kBokehSamples;
				float area = outer * outer - inner * inner; // of the unit disk's
				table[binIt] = std::clamp(static_cast<float>(counts[binIt]) / kSamples / area, 0.0F, 8.0F);
			}
		};
		profile(1.0F, appearance.bokehNear);
		profile(-1.0F, appearance.bokehFar);
	}

	// the lateral chromatic aberration: the chief ray's (through the stop's center) height on the sensor per wavelength,
	// the red's and the blue's over the green's
	auto imageHeight = [&lens, stopRadius](float wavelength, float angle)
	{
		auto traced = Trace(lens, wavelength, stopRadius);
		float chief = A(traced.toStop) != 0.0F ? -B(traced.toStop) * angle / A(traced.toStop) : 0.0F;
		return A(traced.toSensor) * chief + B(traced.toSensor) * angle;
	};
	float angle = std::max(appearance.fieldAngle, 1e-3F);
	float greenHeight = imageHeight(kWavelengths[1], angle);
	if (std::abs(greenHeight) > 1e-6F)
	{
		appearance.redScale = imageHeight(kWavelengths[0], angle) / greenHeight;
		appearance.blueScale = imageHeight(kWavelengths[2], angle) / greenHeight;
	}
	return appearance;
}

std::optional<float> TraceRay(const Prescription& lens, float wavelength, float height, float angle, float stopRadius, size_t* cutAt)
{
	if (cutAt != nullptr)
		*cutAt = lens.surfaces.size();
	if (IsPinhole(lens))
		return std::nullopt;
	// the ray's point and direction in the meridional plane (z along the axis, y up), from the first surface's vertex
	// plane, and the index it is in
	double z = 0.0;
	double y = height;
	double dz = std::cos(angle);
	double dy = std::sin(angle);
	double index = 1.0;
	double vertex = 0.0;
	for (size_t surfaceIt = 0; surfaceIt < lens.surfaces.size(); surfaceIt++)
	{
		const auto& surface = lens.surfaces[surfaceIt];
		if (cutAt != nullptr)
			*cutAt = surfaceIt;
		double radius = surface.radius;
		double normalZ = -1.0;
		double normalY = 0.0;
		if (std::abs(radius) > 1e-6)
		{
			// the sphere's center on the axis, radius behind the vertex (positive), and the ray's nearer crossing of it
			double centerZ = vertex + radius;
			double pz = z - centerZ;
			double b = pz * dz + y * dy;
			double c = pz * pz + y * y - radius * radius;
			double discriminant = b * b - c;
			if (discriminant < 0.0)
				return std::nullopt;
			double root = std::sqrt(discriminant);
			// the crossing on the vertex's side of the center
			double t1 = -b - root;
			double t2 = -b + root;
			double t = radius > 0.0 ? t1 : t2;
			if (t < -1e-6)
				t = radius > 0.0 ? t2 : t1;
			z += t * dz;
			y += t * dy;
			normalZ = (z - centerZ) / radius;
			normalY = y / radius;
			// against the ray
			if (normalZ * dz + normalY * dy > 0.0)
			{
				normalZ = -normalZ;
				normalY = -normalY;
			}
		}
		else
		{
			if (std::abs(dz) < 1e-9)
				return std::nullopt;
			double t = (vertex - z) / dz;
			z = vertex;
			y += t * dy;
		}
		double limit = surfaceIt == lens.stopSurface && stopRadius > 0.0F ? stopRadius : surface.clearRadius;
		if (std::abs(y) > limit)
			return std::nullopt;
		// Snell's law: the direction refracted about the normal
		double next = IndexAt(surface.index, surface.abbe, wavelength);
		double eta = index / next;
		double cosIncidence = -(normalZ * dz + normalY * dy);
		double sinTransmitted2 = eta * eta * (1.0 - cosIncidence * cosIncidence);
		if (sinTransmitted2 > 1.0)
			return std::nullopt;
		double scale = eta * cosIncidence - std::sqrt(1.0 - sinTransmitted2);
		dz = eta * dz + scale * normalZ;
		dy = eta * dy + scale * normalY;
		index = next;
		vertex += surface.thickness;
	}
	// to the sensor, at the last surface's thickness past it
	if (std::abs(dz) < 1e-9)
		return std::nullopt;
	double t = (vertex - z) / dz;
	return static_cast<float>(y + t * dy);
}

float Reflectance(float indexBefore, float indexAfter, float wavelength, bool coated, float tuned) noexcept
{
	auto fresnel = [](float n1, float n2) { return (n1 - n2) / (n1 + n2); };
	if (!coated)
	{
		float r = fresnel(indexBefore, indexAfter);
		return r * r;
	}
	// a single layer, a quarter wave at the tuned wavelength, of magnesium fluoride: the two interfaces' amplitudes
	// interfere by the phase of the double pass
	constexpr float kCoating = 1.38F;
	float r1 = fresnel(indexBefore, kCoating);
	float r2 = fresnel(kCoating, indexAfter);
	float phase = std::numbers::pi_v<float> * tuned / wavelength;
	float c = std::cos(phase);
	return (r1 * r1 + r2 * r2 + 2.0F * r1 * r2 * c) / (1.0F + r1 * r1 * r2 * r2 + 2.0F * r1 * r2 * c);
}

std::vector<Streak> Streaks(const Prescription& lens, float fNumber)
{
	std::vector<Streak> streaks;
	if (IsPinhole(lens) || lens.cylinderRadii.empty())
		return streaks;
	float aperture = std::max(fNumber, lens.widestAperture);
	auto curvature = [](float radius) { return radius != 0.0F ? 1.0F / radius : 0.0F; };
	for (size_t first = 0; first < lens.cylinderRadii.size(); first++)
		for (size_t second = first + 1; second < lens.cylinderRadii.size(); second++)
		{
			float vergence = 2.0F * std::abs(curvature(lens.cylinderRadii[second]) - curvature(lens.cylinderRadii[first]));
			if (vergence <= 0.0F)
				continue;
			Streak streak;
			// half the blur: the pupil's diameter (f over N) over f, times the image's displacement f^2 times the vergence
			streak.halfLength = 0.5F * lens.focalLength * lens.focalLength * vergence / aperture;
			for (size_t channel = 0; channel < kWavelengths.size(); channel++)
			{
				float reflectance = Reflectance(1.0F, lens.cylinderIndex, kWavelengths[channel], lens.coated, lens.cylinderCoating);
				streak.reflectance[static_cast<glm::length_t>(channel)] = reflectance * reflectance;
			}
			streaks.push_back(streak);
		}
	return streaks;
}

std::vector<Ghost> Ghosts(const Prescription& lens, float fNumber)
{
	std::vector<Ghost> ghosts;
	if (IsPinhole(lens))
		return ghosts;
	(void)fNumber;
	for (uint32_t first = 0; first < lens.surfaces.size(); first++)
	{
		if (!Reflects(lens, first))
			continue;
		for (uint32_t second = first + 1; second < lens.surfaces.size(); second++)
		{
			if (!Reflects(lens, second))
				continue;
			Ghost ghost{.first = first, .second = second};
			for (size_t channel = 0; channel < kWavelengths.size(); channel++)
			{
				float wavelength = kWavelengths[channel];
				GhostPath(lens, first, second, wavelength, ghost.toSensor[channel], ghost.toStop[channel]);
				bool firstCoated = lens.coated && !Cemented(lens, first);
				bool secondCoated = lens.coated && !Cemented(lens, second);
				ghost.reflectance[static_cast<glm::length_t>(channel)] =
					Reflectance(IndexAfter(lens, first, wavelength), IndexBefore(lens, first, wavelength), wavelength, firstCoated) *
					Reflectance(IndexBefore(lens, second, wavelength), IndexAfter(lens, second, wavelength), wavelength, secondCoated);
			}
			ghosts.push_back(ghost);
		}
	}
	return ghosts;
}

} // namespace gfx::lens
