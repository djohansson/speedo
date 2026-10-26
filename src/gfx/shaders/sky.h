// the sky: Earth's atmosphere (SHADER_TYPES_ATMOSPHERE_*) lit by the sun, after Hillaire 2020 ("A Scalable and Production
// Ready Sky and Atmosphere Rendering Technique") and his reference implementation (UnrealEngineSkyAtmosphere), shared by
// the shader modules that draw it: the transmittance to the top of the atmosphere and the light scattered more than once
// are tables made once (EnvironmentSkyTransmittance, EnvironmentSkyMultipleScattering), the sky's view of itself a table
// made whenever the sun moves (EnvironmentSkyView), which the panorama the scene is lit by is drawn from (EnvironmentSky)
// and the backdrop samples (SkyRadiance in ibl.slang). distances in km, the planet's center at the origin, +y up; the
// radiance is per unit of the sun's illuminance at the top of the atmosphere (white), except in the view of itself, which
// is in the renderer's units (times SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE)
#pragma once

static const float kSkyPi = 3.14159265358979;

// what the air is made of at an altitude: its scattering (air and aerosols), the air's and the aerosols' apart (for their
// phase functions), and its extinction (both scatter, aerosols absorb a little, ozone absorbs)
struct SkyMedium
{
	float3 scattering;
	float3 extinction;
	float3 rayleigh;
	float mie;
};

SkyMedium SkyMediumAt(float altitude)
{
	float rayleighDensity = exp(-altitude / SHADER_TYPES_ATMOSPHERE_RAYLEIGH_SCALE_HEIGHT);
	float mieDensity = exp(-altitude / SHADER_TYPES_ATMOSPHERE_MIE_SCALE_HEIGHT);
	float ozoneDensity = max(0.0, 1.0 - abs(altitude - SHADER_TYPES_ATMOSPHERE_OZONE_CENTER) / SHADER_TYPES_ATMOSPHERE_OZONE_HALF_WIDTH);
	SkyMedium medium;
	medium.rayleigh = float3(SHADER_TYPES_ATMOSPHERE_RAYLEIGH_R, SHADER_TYPES_ATMOSPHERE_RAYLEIGH_G, SHADER_TYPES_ATMOSPHERE_RAYLEIGH_B) * rayleighDensity;
	medium.mie = SHADER_TYPES_ATMOSPHERE_MIE_SCATTERING * mieDensity;
	medium.scattering = medium.rayleigh + medium.mie;
	medium.extinction = medium.rayleigh + SHADER_TYPES_ATMOSPHERE_MIE_EXTINCTION * mieDensity +
						float3(SHADER_TYPES_ATMOSPHERE_OZONE_R, SHADER_TYPES_ATMOSPHERE_OZONE_G, SHADER_TYPES_ATMOSPHERE_OZONE_B) * ozoneDensity;
	return medium;
}

// the distance along a ray (from origin, a unit direction) to a sphere around the planet's center: where it crosses it
// ahead, the nearer, -1 if it doesn't
float SkySphere(float3 origin, float3 direction, float radius)
{
	float b = dot(origin, direction);
	float c = dot(origin, origin) - radius * radius;
	float discriminant = b * b - c;
	if (discriminant < 0.0)
		return -1.0;
	float root = sqrt(discriminant);
	if (-b - root >= 0.0)
		return -b - root;
	return -b + root >= 0.0 ? -b + root : -1.0;
}

// the tables' coordinates of a unit value, so that 0 and 1 fall on their first and last texels' centers (bilinear
// lookups never reach past them), and back
float SkySubUv(float unit, float size)
{
	return 0.5 / size + unit * (size - 1.0) / size;
}

float SkyUnit(float uv, float size)
{
	return (uv - 0.5 / size) * size / (size - 1.0);
}

// where a ray from radius r at mu (the cosine of its angle from the vertical) is in the transmittance table (Bruneton's
// mapping: by the distance to the top of the atmosphere along it, and the distance to the horizon), and back
float2 SkyTransmittanceUv(float r, float mu)
{
	float bottom2 = SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS * SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS;
	float top2 = SHADER_TYPES_ATMOSPHERE_TOP_RADIUS * SHADER_TYPES_ATMOSPHERE_TOP_RADIUS;
	float h = sqrt(top2 - bottom2);
	float rho = sqrt(max(r * r - bottom2, 0.0));
	float d = max(-r * mu + sqrt(max(r * r * (mu * mu - 1.0) + top2, 0.0)), 0.0);
	float least = SHADER_TYPES_ATMOSPHERE_TOP_RADIUS - r;
	float most = rho + h;
	return float2(
		SkySubUv(saturate((d - least) / max(most - least, 1e-6)), float(SHADER_TYPES_SKY_TRANSMITTANCE_WIDTH)),
		SkySubUv(saturate(rho / h), float(SHADER_TYPES_SKY_TRANSMITTANCE_HEIGHT)));
}

void SkyTransmittanceParameters(float2 uv, out float r, out float mu)
{
	float bottom2 = SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS * SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS;
	float top2 = SHADER_TYPES_ATMOSPHERE_TOP_RADIUS * SHADER_TYPES_ATMOSPHERE_TOP_RADIUS;
	float h = sqrt(top2 - bottom2);
	float rho = h * saturate(SkyUnit(uv.y, float(SHADER_TYPES_SKY_TRANSMITTANCE_HEIGHT)));
	r = sqrt(rho * rho + bottom2);
	float least = SHADER_TYPES_ATMOSPHERE_TOP_RADIUS - r;
	float most = rho + h;
	float d = least + saturate(SkyUnit(uv.x, float(SHADER_TYPES_SKY_TRANSMITTANCE_WIDTH))) * (most - least);
	mu = d <= 0.0 ? 1.0 : clamp((h * h - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}

// the transmittance from radius r at mu to the top of the atmosphere (through the planet where it is in the way: see
// SkySunVisible)
float3 SkyTransmittance(Texture2D table, SamplerState linear, float r, float mu)
{
	return table.SampleLevel(linear, SkyTransmittanceUv(r, mu), 0.0).rgb;
}

// whether the sun is above the planet's horizon from a point
float SkySunVisible(float3 position, float3 sunDirection)
{
	return SkySphere(position, sunDirection, SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS) >= 0.0 ? 0.0 : 1.0;
}

// the light scattered twice or more toward a point at radius r, the sun at muSun from its vertical, per unit of
// scattering (Hillaire's psi_ms: an isotropic phase, the second order's light over 1 less the share scattered again)
float3 SkyMultipleScattering(Texture2D table, SamplerState linear, float r, float muSun)
{
	float size = float(SHADER_TYPES_SKY_MULTIPLE_SCATTERING_SIDE);
	float altitude = saturate((r - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS) / (SHADER_TYPES_ATMOSPHERE_TOP_RADIUS - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS));
	return table.SampleLevel(linear, float2(SkySubUv(saturate(muSun * 0.5 + 0.5), size), SkySubUv(altitude, size)), 0.0).rgb;
}

// the phase functions: air's (Rayleigh), and the aerosols' (Cornette and Shanks)
float SkyRayleighPhase(float cosine)
{
	return 3.0 / (16.0 * kSkyPi) * (1.0 + cosine * cosine);
}

float SkyMiePhase(float cosine)
{
	float g = SHADER_TYPES_ATMOSPHERE_MIE_G;
	float k = 3.0 / (8.0 * kSkyPi) * (1.0 - g * g) / (2.0 + g * g);
	return k * (1.0 + cosine * cosine) / pow(max(1.0 + g * g - 2.0 * g * cosine, 1e-6), 1.5);
}

// the light the atmosphere scatters toward origin along a ray (a unit direction), up to the ground or the top, in
// sampleCount steps, the sun toward sunDirection: its single scattering by each phase function, and the multiple
// scattering's (see SkyMultipleScattering); where the ray meets the ground, the ground's light, lit by the sun alone
// (Lambertian, SHADER_TYPES_ATMOSPHERE_GROUND_ALBEDO). each step integrated as Hillaire's (the source constant over it,
// the transmittance exponential), so that few steps keep the energy
float3 SkyScatteredLight(
	Texture2D transmittanceTable, Texture2D multipleTable, SamplerState linear, float3 origin, float3 direction, float3 sunDirection, uint sampleCount)
{
	float toBottom = SkySphere(origin, direction, SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS);
	float toTop = SkySphere(origin, direction, SHADER_TYPES_ATMOSPHERE_TOP_RADIUS);
	float end = toBottom >= 0.0 ? (toTop >= 0.0 ? min(toTop, toBottom) : toBottom) : toTop;
	if (end <= 0.0)
		return 0.0;
	float cosine = dot(direction, sunDirection);
	float rayleighPhase = SkyRayleighPhase(cosine);
	float miePhase = SkyMiePhase(cosine);
	float step = end / float(sampleCount);
	float3 light = 0.0;
	float3 transmittance = 1.0;
	for (uint sampleIt = 0; sampleIt < sampleCount; sampleIt++)
	{
		float3 position = origin + direction * ((float(sampleIt) + 0.5) * step);
		float r = length(position);
		float muSun = dot(position / r, sunDirection);
		SkyMedium medium = SkyMediumAt(r - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS);
		float3 sun = SkyTransmittance(transmittanceTable, linear, r, muSun) * SkySunVisible(position, sunDirection);
		float3 source = sun * (medium.rayleigh * rayleighPhase + medium.mie * miePhase) +
						SkyMultipleScattering(multipleTable, linear, r, muSun) * medium.scattering;
		float3 through = exp(-medium.extinction * step);
		light += transmittance * (source - source * through) / max(medium.extinction, 1e-9);
		transmittance *= through;
	}
	if (toBottom >= 0.0 && toBottom <= end)
	{
		float3 ground = origin + direction * toBottom;
		float r = length(ground);
		float muSun = dot(ground / r, sunDirection);
		light += transmittance * SkyTransmittance(transmittanceTable, linear, r, muSun) * saturate(muSun) *
				 (SHADER_TYPES_ATMOSPHERE_GROUND_ALBEDO / kSkyPi);
	}
	return light;
}

// where a view is in the sky's view of itself (SHADER_TYPES_SKY_VIEW_TEXTURE, seen from SHADER_TYPES_ATMOSPHERE_EYE_ALTITUDE):
// by the cosine of its angle from the zenith and of its azimuth from the sun's (the sky is symmetric about the sun's
// vertical plane, so 0 to pi of it), the horizon in the middle row and the rows nearer it closer together (Hillaire's
// mapping: the sky changes most there), and back
float2 SkyViewUv(float viewZenithCos, float sunViewCos)
{
	float r = SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS + SHADER_TYPES_ATMOSPHERE_EYE_ALTITUDE;
	float horizon = sqrt(max(r * r - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS * SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS, 0.0));
	float beta = acos(horizon / r); // from the nadir to the horizon
	float zenithHorizon = kSkyPi - beta;
	float zenithAngle = acos(clamp(viewZenithCos, -1.0, 1.0));
	float v = zenithAngle < zenithHorizon ? (1.0 - sqrt(max(1.0 - zenithAngle / zenithHorizon, 0.0))) * 0.5
										  : sqrt(saturate((zenithAngle - zenithHorizon) / beta)) * 0.5 + 0.5;
	float u = sqrt(saturate(-sunViewCos * 0.5 + 0.5));
	return float2(SkySubUv(u, float(SHADER_TYPES_SKY_VIEW_WIDTH)), SkySubUv(v, float(SHADER_TYPES_SKY_VIEW_HEIGHT)));
}

void SkyViewParameters(float2 uv, out float viewZenithCos, out float sunViewCos)
{
	float u = saturate(SkyUnit(uv.x, float(SHADER_TYPES_SKY_VIEW_WIDTH)));
	float v = saturate(SkyUnit(uv.y, float(SHADER_TYPES_SKY_VIEW_HEIGHT)));
	float r = SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS + SHADER_TYPES_ATMOSPHERE_EYE_ALTITUDE;
	float horizon = sqrt(max(r * r - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS * SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS, 0.0));
	float beta = acos(horizon / r);
	float zenithHorizon = kSkyPi - beta;
	if (v < 0.5)
	{
		float coord = 1.0 - 2.0 * v;
		viewZenithCos = cos(zenithHorizon * (1.0 - coord * coord));
	}
	else
	{
		float coord = 2.0 * v - 1.0;
		viewZenithCos = cos(zenithHorizon + beta * coord * coord);
	}
	sunViewCos = 1.0 - 2.0 * u * u;
}

// the sky's radiance in a direction (a unit vector, +y up), the sun toward sunDirection, from its view of itself, without
// the sun's disk (per unit of the sun's illuminance)
float3 SkyViewRadiance(Texture2D view, SamplerState linear, float3 direction, float3 sunDirection)
{
	float2 across = direction.xz;
	float2 sunAcross = sunDirection.xz;
	float lengths = length(across) * length(sunAcross);
	float sunViewCos = lengths > 1e-6 ? dot(across, sunAcross) / lengths : 1.0;
	return view.SampleLevel(linear, SkyViewUv(direction.y, sunViewCos), 0.0).rgb;
}
