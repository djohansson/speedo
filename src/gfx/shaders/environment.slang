// the environment's prefiltering on the gpu (the Environment layout, see gfx::EnvironmentFilter): ports of
// gfx::environment::Prefilter's steps (environment.cpp, the reference the tests check), each a kernel writing one mip
// level of an output through gOutput. the source is the panorama without its dominant lights (they light the scene as
// directional lights), with its pyramid down to 8 texels wide: uploaded for a file, drawn by EnvironmentSky for the
// sky (see sky.h).
module environment;

#include "capi.h"

// the source's pyramid (all of its mips), sampled with gSampler (linear, around in u, clamped in v)
[[vk::binding(0, 1)]]
Texture2D gSource;
[[vk::binding(1, 1)]]
SamplerState gSampler;
// one mip of a texture, read by texel (EnvironmentDownsample's level above, EnvironmentCopy's panorama)
[[vk::binding(2, 1)]]
Texture2D gInput;
// the mip level written
[[vk::binding(3, 1)]]
RWTexture2D<float4> gOutput;
// gEnvironment: the sky's sun (read), and the irradiance (written by EnvironmentIrradiance)
[[vk::binding(4, 1)]]
RWStructuredBuffer<EnvironmentData> gEnvironment;

// set 0 is the push constants' (as in the main layout), the resources are in set 1
[[vk::binding(0, 0)]]
[[vk::push_constant]]
ConstantBuffer<EnvironmentFilterConstants> gFilter;

// the sky's tables (see sky.h), sampled with gSampler at their texels' centers and between them (SkySubUv: never past
// their edges, so its wrapping in u doesn't matter): the transmittance, the multiple scattering, and the sky's view of
// itself, each written through gOutput by its kernel before the next reads it
[[vk::binding(5, 1)]]
Texture2D gSkyTransmittance;
[[vk::binding(6, 1)]]
Texture2D gSkyMultipleScattering;
[[vk::binding(7, 1)]]
Texture2D gSkyView;

#include "sky.h"

static const float kPi = 3.14159265358979;

// the direction at a point of the panorama (see gfx::environment::Panorama), and back
float3 Direction(float2 uv)
{
	float phi = (uv.x - 0.5) * 2.0 * kPi;
	float theta = uv.y * kPi;
	return float3(sin(theta) * sin(phi), cos(theta), -sin(theta) * cos(phi));
}

float2 Coordinates(float3 d)
{
	return float2(0.5 + atan2(d.x, -d.z) / (2.0 * kPi), acos(clamp(d.y, -1.0, 1.0)) / kPi);
}

float2 Hammersley(uint i, uint count)
{
	return float2(float(i) / float(count), float(reversebits(i)) * 2.3283064365386963e-10);
}

// the output texel a thread writes (none outside the level), and its center's coordinates
bool OutputTexel(uint2 id, out float2 uv)
{
	uint width;
	uint height;
	gOutput.GetDimensions(width, height);
	uv = (float2(id) + 0.5) / float2(width, height);
	return id.x < width && id.y < height;
}

float3 SampleSource(float lod, float2 uv)
{
	return gSource.SampleLevel(gSampler, uv, clamp(lod, 0.0, float(gFilter.sourceLevelCount - 1u))).rgb;
}

// a texel's solid angle at the equator of the source's level 0
float EquatorTexelSolidAngle()
{
	uint width;
	uint height;
	uint levels;
	gSource.GetDimensions(0, width, height, levels);
	return (2.0 * kPi / float(width)) * (kPi / float(height));
}

float SourceHeight()
{
	uint width;
	uint height;
	uint levels;
	gSource.GetDimensions(0, width, height, levels);
	return float(height);
}

// the sky's transmittance table (SHADER_TYPES_SKY_TRANSMITTANCE_WIDTH by _HEIGHT, see SkyTransmittanceUv): from each
// radius and angle to the top of the atmosphere, the optical depth in 40 steps
[shader("compute")]
[numthreads(8, 8, 1)]
void EnvironmentSkyTransmittance(uint3 id : SV_DispatchThreadID)
{
	float2 uv;
	if (!OutputTexel(id.xy, uv))
		return;
	float r;
	float mu;
	SkyTransmittanceParameters(uv, r, mu);
	float3 origin = float3(0.0, r, 0.0);
	float3 direction = float3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);
	float end = max(SkySphere(origin, direction, SHADER_TYPES_ATMOSPHERE_TOP_RADIUS), 0.0);
	const uint kStepCount = 40;
	float step = end / float(kStepCount);
	float3 depth = 0.0;
	for (uint stepIt = 0; stepIt < kStepCount; stepIt++)
	{
		float3 position = origin + direction * ((float(stepIt) + 0.5) * step);
		depth += SkyMediumAt(length(position) - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS).extinction * step;
	}
	gOutput[id.xy] = float4(exp(-depth), 1.0);
}

// the sky's multiple scattering table (SHADER_TYPES_SKY_MULTIPLE_SCATTERING_SIDE square, see SkyMultipleScattering): from
// each altitude, the sun at each angle, the light scattered once toward it over 64 directions and the share of light
// scattered again (an isotropic phase, Hillaire's approximation), in 20 steps each; serially, without shared memory (the
// tests run the kernels on the cpu, where a group's threads run one after another)
[shader("compute")]
[numthreads(8, 8, 1)]
void EnvironmentSkyMultipleScattering(uint3 id : SV_DispatchThreadID)
{
	float2 uv;
	if (!OutputTexel(id.xy, uv))
		return;
	float size = float(SHADER_TYPES_SKY_MULTIPLE_SCATTERING_SIDE);
	float muSun = saturate(SkyUnit(uv.x, size)) * 2.0 - 1.0;
	float r = SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS + max(saturate(SkyUnit(uv.y, size)) *
		(SHADER_TYPES_ATMOSPHERE_TOP_RADIUS - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS), SHADER_TYPES_ATMOSPHERE_EYE_ALTITUDE);
	float3 origin = float3(0.0, r, 0.0);
	float3 sunDirection = float3(sqrt(max(1.0 - muSun * muSun, 0.0)), muSun, 0.0);
	const uint kSideCount = 8;
	const uint kStepCount = 20;
	float isotropic = 1.0 / (4.0 * kSkyPi);
	float3 once = 0.0; // the light scattered once toward the point, over the directions
	float3 again = 0.0; // the share of light scattered toward the point, an isotropic source's
	for (uint directionIt = 0; directionIt < kSideCount * kSideCount; directionIt++)
	{
		float theta = 2.0 * kSkyPi * (float(directionIt % kSideCount) + 0.5) / float(kSideCount);
		float cosPhi = 1.0 - 2.0 * (float(directionIt / kSideCount) + 0.5) / float(kSideCount);
		float sinPhi = sqrt(max(1.0 - cosPhi * cosPhi, 0.0));
		float3 direction = float3(cos(theta) * sinPhi, cosPhi, sin(theta) * sinPhi);
		float toBottom = SkySphere(origin, direction, SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS);
		float toTop = SkySphere(origin, direction, SHADER_TYPES_ATMOSPHERE_TOP_RADIUS);
		float end = toBottom >= 0.0 ? (toTop >= 0.0 ? min(toTop, toBottom) : toBottom) : toTop;
		if (end <= 0.0)
			continue;
		float step = end / float(kStepCount);
		float3 transmittance = 1.0;
		for (uint stepIt = 0; stepIt < kStepCount; stepIt++)
		{
			float3 position = origin + direction * ((float(stepIt) + 0.5) * step);
			float radius = length(position);
			SkyMedium medium = SkyMediumAt(radius - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS);
			float3 sun = SkyTransmittance(gSkyTransmittance, gSampler, radius, dot(position / radius, sunDirection)) *
						 SkySunVisible(position, sunDirection);
			float3 through = exp(-medium.extinction * step);
			float3 scattered = transmittance * (medium.scattering - medium.scattering * through) / max(medium.extinction, 1e-9);
			once += scattered * sun * isotropic;
			again += scattered;
			transmittance *= through;
		}
		if (toBottom >= 0.0 && toBottom <= end)
		{
			float3 ground = origin + direction * toBottom;
			float radius = length(ground);
			float muGround = dot(ground / radius, sunDirection);
			once += transmittance * SkyTransmittance(gSkyTransmittance, gSampler, radius, muGround) * saturate(muGround) *
					(SHADER_TYPES_ATMOSPHERE_GROUND_ALBEDO / kSkyPi);
		}
	}
	// the means over the sphere (its solid angle times the isotropic phase is 1), and every order past the second as a
	// geometric series of the share scattered again
	float count = float(kSideCount * kSideCount);
	once /= count;
	again /= count;
	gOutput[id.xy] = float4(once / max(1.0 - again, 1e-3), 1.0);
}

// the sky's view of itself (SHADER_TYPES_SKY_VIEW_WIDTH by _HEIGHT, see SkyViewUv), the sun at its angle in
// gEnvironment[0].skySun: the light scattered toward the eye along each view, and the ground's below the horizon, in the
// renderer's units (times SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE)
[shader("compute")]
[numthreads(8, 8, 1)]
void EnvironmentSkyView(uint3 id : SV_DispatchThreadID)
{
	float2 uv;
	if (!OutputTexel(id.xy, uv))
		return;
	float viewZenithCos;
	float sunViewCos;
	SkyViewParameters(uv, viewZenithCos, sunViewCos);
	float muSun = clamp(gEnvironment[0].skySun.y, -1.0, 1.0);
	float3 sunDirection = float3(sqrt(max(1.0 - muSun * muSun, 0.0)), muSun, 0.0);
	float sinZenith = sqrt(max(1.0 - viewZenithCos * viewZenithCos, 0.0));
	float3 direction = float3(sinZenith * sunViewCos, viewZenithCos, sinZenith * sqrt(max(1.0 - sunViewCos * sunViewCos, 0.0)));
	float3 origin = float3(0.0, SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS + SHADER_TYPES_ATMOSPHERE_EYE_ALTITUDE, 0.0);
	float3 light = SkyScatteredLight(gSkyTransmittance, gSkyMultipleScattering, gSampler, origin, direction, sunDirection, 32);
	gOutput[id.xy] = float4(light * SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE, 1.0);
}

// the sky without its sun (a directional light, see gfx::environment::SunIrradiance): the source's level 0, from its view
// of itself
[shader("compute")]
[numthreads(8, 8, 1)]
void EnvironmentSky(uint3 id : SV_DispatchThreadID)
{
	float2 uv;
	if (OutputTexel(id.xy, uv))
		gOutput[id.xy] = float4(SkyViewRadiance(gSkyView, gSampler, Direction(uv), gEnvironment[0].skySun.xyz), 1.0);
}

// a mip of the source's pyramid: each texel the mean of the four above it by their solid angles (rows nearer the poles
// cover less of the sphere), so that every mip keeps the panorama's mean (as gfx::environment's Pyramid)
[shader("compute")]
[numthreads(8, 8, 1)]
void EnvironmentDownsample(uint3 id : SV_DispatchThreadID)
{
	float2 uv;
	if (!OutputTexel(id.xy, uv))
		return;
	uint width;
	uint height;
	gInput.GetDimensions(width, height);
	float top = sin((float(2u * id.y) + 0.5) / float(height) * kPi);
	float bottom = sin((float(2u * id.y + 1u) + 0.5) / float(height) * kPi);
	int2 at = int2(id.xy) * 2;
	float3 color = top * (gInput.Load(int3(at, 0)).rgb + gInput.Load(int3(at + int2(1, 0), 0)).rgb) +
		bottom * (gInput.Load(int3(at + int2(0, 1), 0)).rgb + gInput.Load(int3(at + int2(1, 1), 0)).rgb);
	gOutput[id.xy] = float4(color * (0.5 / (top + bottom)), 1.0);
}

// a level as it is: the prefiltered levels' level 0 (the panorama, dominant lights included)
[shader("compute")]
[numthreads(8, 8, 1)]
void EnvironmentCopy(uint3 id : SV_DispatchThreadID)
{
	float2 uv;
	if (OutputTexel(id.xy, uv))
		gOutput[id.xy] = float4(gInput.Load(int3(id.xy, 0)).rgb, 1.0);
}

// a GGX prefiltered level, for gFilter.roughness (see gfx::environment's PrefilterTexel): the source weighted by the lobe
// around n (n = v = r), sampled by importance from its mips by each sample's solid angle
[shader("compute")]
[numthreads(8, 8, 1)]
void EnvironmentSpecular(uint3 id : SV_DispatchThreadID)
{
	float2 uv;
	if (!OutputTexel(id.xy, uv))
		return;

	const uint kSampleCount = 128;
	float3 n = Direction(uv);
	float alpha = gFilter.roughness * gFilter.roughness;
	float alpha2 = alpha * alpha;
	float3 up = abs(n.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	float3 tangent = normalize(cross(up, n));
	float3 bitangent = cross(n, tangent);
	float equatorTexelSolidAngle = EquatorTexelSolidAngle();
	float height = SourceHeight();

	float3 sum = 0.0;
	float weight = 0.0;
	for (uint i = 0; i < kSampleCount; i++)
	{
		float2 xi = Hammersley(i, kSampleCount);
		float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha2 - 1.0) * xi.y));
		float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
		float phi = 2.0 * kPi * xi.x;
		float3 h = tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + n * cosTheta;
		float3 l = 2.0 * dot(n, h) * h - n;
		float nDotL = dot(n, l);
		if (nDotL <= 0.0)
			continue;

		// pdf of l: D(h) * nDotH / (4 vDotH), with v = n
		float d = cosTheta * cosTheta * (alpha2 - 1.0) + 1.0;
		float pdf = alpha2 / (kPi * d * d) / 4.0;
		float sampleSolidAngle = 1.0 / (float(kSampleCount) * pdf + 1e-6);
		float2 sampleUv = Coordinates(l);
		float texelSolidAngle = equatorTexelSolidAngle * max(sin(sampleUv.y * kPi), 1.0 / height);
		float lod = 0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0;

		sum += SampleSource(lod, sampleUv) * nDotL;
		weight += nDotL;
	}
	gOutput[id.xy] = float4(weight > 0.0 ? sum / weight : SampleSource(0.0, uv), 1.0);
}

// a sheen (Charlie) prefiltered level, for gFilter.roughness (see gfx::environment's PrefilterSheenTexel): l sampled over
// the hemisphere with cos(theta) = u^k, more of them near the horizon the lower the roughness, weighted by D(h) n.l
[shader("compute")]
[numthreads(8, 8, 1)]
void EnvironmentSheen(uint3 id : SV_DispatchThreadID)
{
	float2 uv;
	if (!OutputTexel(id.xy, uv))
		return;

	const uint kSampleCount = 512;
	float3 n = Direction(uv);
	float clamped = max(gFilter.roughness, 0.07);
	float inverseAlpha = 1.0 / (clamped * clamped);
	float3 up = abs(n.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	float3 tangent = normalize(cross(up, n));
	float3 bitangent = cross(n, tangent);
	float equatorTexelSolidAngle = EquatorTexelSolidAngle();
	float height = SourceHeight();
	float exponent = 1.0 + 0.1 * inverseAlpha;

	float3 sum = 0.0;
	float weight = 0.0;
	for (uint i = 0; i < kSampleCount; i++)
	{
		float2 xi = Hammersley(i, kSampleCount);
		float cosTheta = pow(1.0 - xi.y, exponent);
		float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
		float phi = 2.0 * kPi * xi.x;
		float3 l = tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + n * cosTheta;

		// D(h) relative to its maximum on the hemisphere, over the density of cos(theta) (u^k's: c^(1 / k - 1) / k)
		float sinHalf = sqrt(max(0.0, (1.0 - cosTheta) * 0.5));
		float pdf = pow(cosTheta, 1.0 / exponent - 1.0) / exponent;
		float w = pow(sinHalf * 1.41421356, inverseAlpha) * cosTheta / pdf;
		if (!(w > 0.0))
			continue;
		float sampleSolidAngle = 2.0 * kPi / (float(kSampleCount) * pdf);
		float2 sampleUv = Coordinates(l);
		float texelSolidAngle = equatorTexelSolidAngle * max(sin(sampleUv.y * kPi), 1.0 / height);
		float lod = 0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0;

		sum += SampleSource(lod, sampleUv) * w;
		weight += w;
	}
	gOutput[id.xy] = float4(weight > 0.0 ? sum / weight : SampleSource(0.0, uv), 1.0);
}

// the irradiance as 9 spherical harmonics coefficients into gEnvironment[0] (see gfx::environment::EvaluateIrradiance):
// the radiance of gInput (a mip of the source at most 128 wide) projected on the basis by each texel's solid angle,
// convolved with the clamped cosine and divided by pi. a thread per coefficient, without shared memory (the tests run the
// kernels on the cpu, where a group's threads run one after another)
float ShBasis(uint c, float3 d)
{
	switch (c)
	{
	case 0: return 0.282095;
	case 1: return 0.488603 * d.y;
	case 2: return 0.488603 * d.z;
	case 3: return 0.488603 * d.x;
	case 4: return 1.092548 * d.x * d.y;
	case 5: return 1.092548 * d.y * d.z;
	case 6: return 0.315392 * (3.0 * d.z * d.z - 1.0);
	case 7: return 1.092548 * d.x * d.z;
	default: return 0.546274 * (d.x * d.x - d.y * d.y);
	}
}

[shader("compute")]
[numthreads(16, 1, 1)]
void EnvironmentIrradiance(uint3 id : SV_GroupThreadID)
{
	uint c = id.x;
	if (c >= 9)
		return;

	uint width;
	uint height;
	gInput.GetDimensions(width, height);
	float3 sum = 0.0;
	for (uint y = 0; y < height; y++)
	{
		float v = (float(y) + 0.5) / float(height);
		float solidAngle = (2.0 * kPi / float(width)) * (kPi / float(height)) * sin(v * kPi);
		for (uint x = 0; x < width; x++)
		{
			float3 d = Direction(float2((float(x) + 0.5) / float(width), v));
			sum += gInput.Load(int3(x, y, 0)).rgb * (solidAngle * ShBasis(c, d));
		}
	}
	float band = c == 0 ? 1.0 : c < 4 ? 2.0 / 3.0 : 0.25;
	gEnvironment[0].irradiance[c] = float4(sum * band, 0.0);
}
