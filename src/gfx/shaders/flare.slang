// the glare's flares (the Flare layout, see the Flare pass in gfx/renderer.cpp): the ghosts and the starburst of each
// source the frame holds (LensSource, listed by ComputeFlareSources: the sun's disk, a lamp), drawn over the lens's
// image, added: a quad an instance, each source's ghosts (LensGhost: a pair of the lens's surfaces reflecting a ray back
// and forward again, Lee and Eisemann 2013) then its starburst (the aperture's blades' diffraction), the quad over what
// the fragment stage draws, which it works out from the pixel's place on the sensor. in the renderer's units of
// radiance, as the lens's image is: a ghost's irradiance on the sensor is the source's at the lens (its radiance times
// its solid angle) times the two surfaces' reflectances over the square of the ghost path's magnification, and a
// sensor irradiance reads as the radiance that gives it through the pupil, 4 N^2 over pi of it

#include "capi.h"

[[vk::binding(0, 0)]]
[[vk::push_constant]]
ConstantBuffer<FlareConstants> gFlare;

// set 1's resources, a parameter block's fields in their bindings' order, named as gfx binds them (see bindings.slang)
struct FlareSet
{
	// the lens (LensData) and its ghosts, per frame index (the renderer's gLensData and gLensGhosts)
	StructuredBuffer<LensData> gFlareLens[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<LensGhost> gFlareGhosts[SHADER_TYPES_FRAME_COUNT];
	// the frame's sources: how many, then the sources (gLensSourceCount, gLensSources)
	StructuredBuffer<uint> gFlareSourceCount;
	StructuredBuffer<LensSource> gFlareSources;
};
[[vk::binding(0, 1)]]
ParameterBlock<FlareSet> gSetFlare;
#define gFlareLens gSetFlare.gFlareLens
#define gFlareGhosts gSetFlare.gFlareGhosts
#define gFlareSourceCount gSetFlare.gFlareSourceCount
#define gFlareSources gSetFlare.gFlareSources

static const float kFlareLeastVisible = 1e-3; // of white at the view's exposure (lens.slang's)
static const float kPi = 3.14159265;
// the wavelengths the channels stand for (lens::kWavelengths), mm
static const float3 kFlareWavelengths = float3(650.0e-6, 550.0e-6, 450.0e-6);

float FlareLuminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// the sensor's millimeters per output pixel of a view (LensSensorMmPerPixel in lens.slang)
float2 FlareSensorMmPerPixel(LensData lens, float4 viewport)
{
	float2 toSensor = float2(1.0 / max(lens.squeeze, 1.0), 1.0);
	float halfDiagonalPixels = 0.5 * length(viewport.zw * toSensor);
	return toSensor * lens.halfDiagonal / max(halfDiagonalPixels, 1.0);
}

// a sensor irradiance as the radiance that gives it through the pupil: f^2 over pi times the entrance pupil's radius
// squared (4 N^2 / pi)
float FlareIrradianceToRadiance(LensData lens)
{
	return lens.focalLength * lens.focalLength / (kPi * max(lens.pupilRadius * lens.pupilRadius, 1e-6));
}

// the barrel's round opening the blades' polygon is cut by (LensData::openRadius; none for the pinhole)
float FlareOpenRadius(LensData lens)
{
	return lens.openRadius > 0.0 ? lens.openRadius : 1.0e30;
}

// the blades' polygon's edges' straight length (mm; lens::StraightEdgeLength): the stop's radius its apothem, its edge
// 2 a tan(pi / N), or the chord the opening leaves of it (none wide open, where the aperture is round)
float FlareEdgeLength(LensData lens)
{
	if (lens.bladeCount < 3u)
		return 0.0;
	float open = FlareOpenRadius(lens);
	float edge = 2.0 * lens.stopRadius * tan(kPi / float(lens.bladeCount));
	float chord = 2.0 * sqrt(max(open * open - lens.stopRadius * lens.stopRadius, 0.0));
	return min(edge, chord);
}

// how far a point of the stop's plane is outside the aperture (the blades' polygon, the nearest edge's line, cut by the
// round opening; a circle of the stop's radius without blades), mm
float FlarePolygonDistance(float2 at, LensData lens)
{
	if (lens.bladeCount < 3u)
		return length(at) - lens.stopRadius;
	float sector = 2.0 * kPi / float(lens.bladeCount);
	float angle = atan2(at.y, at.x);
	float nearest = round(angle / sector) * sector;
	return max(dot(at, float2(cos(nearest), sin(nearest))) - lens.stopRadius, length(at) - FlareOpenRadius(lens));
}

// a ghost's irradiance at a point of the sensor, per channel (see the header: the ray entering at the height that lands
// there must pass the first surface's clear radius and, where it first crosses the stop's plane, the blades' polygon;
// the edges antialiased over a pixel of the sensor), as radiance
float3 FlareGhost(LensData lens, LensGhost ghost, LensSource source, float2 sensor, float pixelMm)
{
	float2 angle = source.position.xy / max(lens.focalLength, 1e-3);
	float toRadiance = FlareIrradianceToRadiance(lens);
	float3 color = 0.0;
	[unroll]
	for (uint channel = 0; channel < 3; channel++)
	{
		float4 m = ghost.sensor[channel]; // a, b (to the sensor), as, bs (to the stop)
		if (abs(m.x) < 1e-6)
			continue;
		float2 entrance = (sensor - m.y * angle) / m.x;
		float2 stop = m.z * entrance + m.w * angle;
		// how far outside, in the sensor's mm (the entrance's heights magnified by a onto it, the stop's by a over as)
		float outsideEntrance = (length(entrance) - lens.entranceRadius) * abs(m.x);
		float outsideStop = FlarePolygonDistance(stop, lens) * abs(m.x) / max(abs(m.z), 1e-4);
		float coverage = saturate(0.5 - max(outsideEntrance, outsideStop) / pixelMm);
		color[channel] = source.flux[channel] * ghost.reflectance[channel] / (m.x * m.x) * toRadiance * coverage;
	}
	return color;
}

// the share of a spike at a distance along it (mm) the sprites draw: its far field, past half the near field's reach
// (see ComputeStarburst, which gathers every pixel's spikes as far as SHADER_TYPES_STAR_REACH half texels, fading out
// over the second half: here the complement, by the same steps, two output pixels each along the spike's direction)
float FlareFarField(float along, float2 normal, float2 mmPerPixel)
{
	float stepMm = length(2.0 * normalize(normal / mmPerPixel) * mmPerPixel);
	return smoothstep(0.5 * float(SHADER_TYPES_STAR_REACH), float(SHADER_TYPES_STAR_REACH), along / max(stepMm, 1e-6));
}

// the starburst's far field at a point of the sensor at an offset from the source, per channel: each edge of the blades'
// polygon diffracts a streak either way along its normal, falling as the source's irradiance times the edge's length
// squared over 4 pi^2 times the distance squared (the far field of an edge), as wide across as the wavelength times the
// focal length over the edge (its first zero), and here a gaussian of that integral at least a pixel wide; past the
// near field, which every pixel's spikes are gathered over (FlareFarField)
float3 FlareStarburst(LensData lens, LensSource source, float2 offset, float2 mmPerPixel)
{
	float pixelMm = max(mmPerPixel.x, mmPerPixel.y);
	float edge = FlareEdgeLength(lens);
	if (edge <= 0.0)
		return 0.0;
	float fNumber = lens.focalLength / max(2.0 * lens.pupilRadius, 1e-3);
	float axis = edge * edge * fNumber * fNumber / (kPi * kPi * kPi); // over the distance squared: radiance
	float3 width = kFlareWavelengths * lens.focalLength / edge;
	// across: the edge's diffraction (at least a pixel), spread by the source's disk (LensSource::flux.w, see
	// ComputeFlareSize: a uniform disk's chord across has a deviation of half its radius), the integral across kept
	float spread = 0.5 * source.flux.w;
	float3 sigma = sqrt(max(width / 2.5, 0.6 * pixelMm) * max(width / 2.5, 0.6 * pixelMm) + spread * spread);
	float3 scale = axis * width / (sigma * sqrt(2.0 * kPi));
	float3 color = 0.0;
	float sector = 2.0 * kPi / float(lens.bladeCount);
	for (uint edgeIt = 0; edgeIt < lens.bladeCount; edgeIt++)
	{
		float2 normal = float2(cos(float(edgeIt) * sector), sin(float(edgeIt) * sector));
		float along = abs(dot(offset, normal));
		float across = abs(dot(offset, float2(-normal.y, normal.x)));
		float far = FlareFarField(along, normal, mmPerPixel);
		if (far <= 0.0)
			continue;
		color += scale * far * exp(-0.5 * across * across / (sigma * sigma)) / (along * along);
	}
	// an anamorphic lens's cylindrical front element: a streak across (along the squeeze) from its edge, the lens's
	// whole clear aperture, as the blades' edges give theirs, tinted blue as its coatings reflect it (the streak the
	// lenses are known for)
	if (lens.squeeze > 1.0)
	{
		float cylinderEdge = 2.0 * lens.entranceRadius;
		float cylinderAxis = cylinderEdge * cylinderEdge * fNumber * fNumber / (kPi * kPi * kPi);
		float3 cylinderWidth = kFlareWavelengths * lens.focalLength / cylinderEdge;
		float3 cylinderSigma = sqrt(max(cylinderWidth / 2.5, 0.6 * pixelMm) * max(cylinderWidth / 2.5, 0.6 * pixelMm) + spread * spread);
		float3 cylinderScale = cylinderAxis * cylinderWidth / (cylinderSigma * sqrt(2.0 * kPi)) * float3(0.55, 0.75, 1.0);
		float along = abs(offset.x);
		float across = abs(offset.y);
		float far = FlareFarField(along, float2(1.0, 0.0), mmPerPixel);
		if (far > 0.0)
			color += cylinderScale * far * exp(-0.5 * across * across / (cylinderSigma * cylinderSigma)) / (along * along);
	}
	return color * source.flux.rgb;
}

struct FlareVertexOutput
{
	float4 position : SV_Position;
	nointerpolation uint source : SOURCE;
	nointerpolation uint item : ITEM; // the source's ghost, or gFlare.ghostCount for its starburst
};

// the quad's corners (two triangles, 6 vertices)
static const float2 kFlareCorners[6] = {float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0), float2(1.0, 1.0), float2(-1.0, 1.0), float2(-1.0, -1.0)};

// an instance's quad: its source's ghost or starburst over its reach on the sensor (a ghost's disk, the first surface's
// clear radius magnified; the starburst as far as its streaks show), in the output's clip space (y down, as the pixels
// are), cut to the view's viewport; none for a source past the frame's count or a ghost too dim to show
[shader("vertex")]
FlareVertexOutput VertexFlare(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
	FlareVertexOutput output;
	uint itemCount = gFlare.ghostCount + 1u;
	output.source = instanceId / itemCount;
	output.item = instanceId % itemCount;
	output.position = float4(0.0, 0.0, 0.0, 1.0);
	LensData lens = gFlareLens[gFlare.frame][0];
	// the frame's list of the sources tracked (see ComputeFlareTrack), its light by its strength
	uint parity = lens.grainFrame & 1u;
	if (output.source >= min(gFlareSourceCount[parity], SHADER_TYPES_LENS_SOURCE_COUNT))
		return output;
	LensSource source = gFlareSources[parity * SHADER_TYPES_LENS_SOURCE_COUNT + output.source];
	source.flux.rgb *= source.position.w;
	float2 mmPerPixel = FlareSensorMmPerPixel(lens, source.viewport);
	float pixelMm = max(mmPerPixel.x, mmPerPixel.y);
	float exposure = source.position.z;
	float2 angle = source.position.xy / max(lens.focalLength, 1e-3);
	float2 center;
	float2 extent;
	if (output.item < gFlare.ghostCount)
	{
		LensGhost ghost = gFlareGhosts[gFlare.frame][output.item];
		float4 red = ghost.sensor[0];
		float4 green = ghost.sensor[1];
		float4 blue = ghost.sensor[2];
		// what the ghost can cover: the entrance's image (a disk of the first surface's clear radius, magnified by a, at
		// b t) within the stop's (its polygon's circle, magnified by a over as, at (b - a bs / as) t): the box of the
		// two disks' overlap, none where they don't meet (a quad over the whole entrance's image, mostly cut away by
		// the stop, cost 2.5 ms of fill at 720p for the sun's 45 ghosts)
		float2 entranceCenter = green.y * angle;
		float entranceRadius = max(abs(green.x), max(abs(red.x), abs(blue.x))) * lens.entranceRadius;
		float2 low = entranceCenter - entranceRadius;
		float2 high = entranceCenter + entranceRadius;
		if (abs(green.z) > 1e-4)
		{
			float2 stopCenter = (green.y - green.x * green.w / green.z) * angle;
			float stopCircle = lens.bladeCount >= 3u ? min(lens.stopRadius / cos(kPi / float(lens.bladeCount)), FlareOpenRadius(lens)) : lens.stopRadius;
			float stopRadius = abs(green.x / green.z) * stopCircle;
			low = max(low, stopCenter - stopRadius);
			high = min(high, stopCenter + stopRadius);
		}
		float apart = max(abs(red.y - green.y), abs(blue.y - green.y)) * length(angle);
		low -= apart + 2.0 * pixelMm;
		high += apart + 2.0 * pixelMm;
		if (any(high <= low))
			return output;
		center = 0.5 * (low + high);
		extent = 0.5 * (high - low);
		float gain = 0.0;
		[unroll]
		for (uint channel = 0; channel < 3; channel++)
			gain = max(gain, ghost.reflectance[channel] / max(ghost.sensor[channel].x * ghost.sensor[channel].x, 1e-6));
		if (FlareLuminance(source.flux.rgb) * exposure * gain * FlareIrradianceToRadiance(lens) < kFlareLeastVisible)
			return output;
	}
	else
	{
		center = source.position.xy;
		// as far as the green's streak falls to kFlareLeastVisible (see FlareStarburst; an anamorphic lens's cylinder's
		// reaches farthest)
		float edge = FlareEdgeLength(lens);
		if (lens.squeeze > 1.0)
			edge = max(edge, 2.0 * lens.entranceRadius);
		if (edge <= 0.0)
			return output;
		float fNumber = lens.focalLength / max(2.0 * lens.pupilRadius, 1e-3);
		float axis = edge * edge * fNumber * fNumber / (kPi * kPi * kPi);
		float width = kFlareWavelengths.y * lens.focalLength / edge;
		float sigma = max(width / 2.5, 0.6 * pixelMm);
		float peak = FlareLuminance(source.flux.rgb) * exposure * axis * width / (sigma * sqrt(2.0 * kPi));
		float reach = sqrt(max(peak, 0.0) / kFlareLeastVisible);
		// none where the spikes fade before their far field (the near field draws them, see FlareFarField)
		if (reach < float(SHADER_TYPES_STAR_REACH) * min(mmPerPixel.x, mmPerPixel.y))
			return output;
		extent = reach;
	}
	float2 sensor = center + kFlareCorners[vertexId] * extent;
	float2 pixel = source.viewport.xy + 0.5 * source.viewport.zw + sensor / mmPerPixel;
	pixel = clamp(pixel, source.viewport.xy, source.viewport.xy + source.viewport.zw);
	float2 clip = pixel / float2(gFlare.width, gFlare.height) * 2.0 - 1.0;
	output.position = float4(clip, 0.0, 1.0);
	return output;
}

[shader("fragment")]
float4 FragmentFlare(FlareVertexOutput input) : SV_Target
{
	LensData lens = gFlareLens[gFlare.frame][0];
	uint parity = lens.grainFrame & 1u;
	LensSource source = gFlareSources[parity * SHADER_TYPES_LENS_SOURCE_COUNT + input.source];
	source.flux.rgb *= source.position.w;
	float2 pixel = input.position.xy;
	if (any(pixel < source.viewport.xy) || any(pixel > source.viewport.xy + source.viewport.zw))
		return float4(0.0, 0.0, 0.0, 0.0);
	float2 mmPerPixel = FlareSensorMmPerPixel(lens, source.viewport);
	float pixelMm = max(mmPerPixel.x, mmPerPixel.y);
	float2 sensor = (pixel - (source.viewport.xy + 0.5 * source.viewport.zw)) * mmPerPixel;
	float3 color = input.item < gFlare.ghostCount ? FlareGhost(lens, gFlareGhosts[gFlare.frame][input.item], source, sensor, pixelMm)
												 : FlareStarburst(lens, source, sensor - source.position.xy, mmPerPixel);
	return float4(max(color, 0.0), 0.0);
}
