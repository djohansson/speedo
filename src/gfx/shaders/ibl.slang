// image based lighting: environment lookups (prefiltered radiance, sheen, irradiance), the sky. a part of shaders.slang, which includes it after capi.h and sky.h

// the environment (see EnvironmentData): a world direction in the panorama's space, turned back by its rotation
float3 EnvironmentDirection(float3 d)
{
	float angle = -gPushConstants.environmentRotation;
	float c = cos(angle);
	float s = sin(angle);
	return float3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

// where a direction (in the panorama's space) is in the panorama (as gfx::environment::Panorama)
float2 EnvironmentCoordinates(float3 d)
{
	return float2(0.5 + atan2(d.x, -d.z) / (2.0 * kPi), acos(clamp(d.y, -1.0, 1.0)) / kPi);
}

// the radiance a white Lambertian surface of normal n reflects (as gfx::environment::EvaluateIrradiance), scaled by the
// intensity
float3 EnvironmentIrradiance(EnvironmentData environment, float3 n)
{
	n = EnvironmentDirection(n);
	float basis[9] = {
		0.282095,
		0.488603 * n.y,
		0.488603 * n.z,
		0.488603 * n.x,
		1.092548 * n.x * n.y,
		1.092548 * n.y * n.z,
		0.315392 * (3.0 * n.z * n.z - 1.0),
		1.092548 * n.x * n.z,
		0.546274 * (n.x * n.x - n.y * n.y),
	};
	float3 result = 0.0;
	for (uint i = 0; i < 9; i++)
		result += environment.irradiance[i].rgb * basis[i];
	return max(result, 0.0) * gPushConstants.environmentIntensity;
}

// the sky's radiance in a direction (in the panorama's space), sun included (see EnvironmentData::skySun): its view of
// itself (see sky.h), and the sun's disk through the atmosphere, its edge soft over a fifth of its radius (a pixel or two;
// the angle as twice the asin of half the chord, which keeps its precision where acos of the cosine loses it)
float3 SkyRadiance(EnvironmentData environment, float3 d)
{
	float3 color = SkyViewRadiance(gTextures[SHADER_TYPES_SKY_VIEW_TEXTURE], gSamplers[environment.samplerId], d, environment.skySun.xyz);
	float radius = acos(environment.skySun.w);
	float angle = 2.0 * asin(min(0.5 * length(d - environment.skySun.xyz), 1.0));
	return color + environment.skySunRadiance.rgb * (1.0 - smoothstep(radius * 0.9, radius * 1.1, angle));
}

// the radiance reflected along r, prefiltered for a roughness (the split sum's first term), scaled by the intensity.
// roughness 0 is the panorama itself, as the backdrop shows it. the sky is evaluated where it is sharper than its
// panorama: below the first prefiltered level's roughness, blending into it
float3 EnvironmentRadiance(EnvironmentData environment, float3 r, float roughness)
{
	float lod = roughness * (environment.levelCount - 1.0);
	float3 d = EnvironmentDirection(r);
	float2 uv = EnvironmentCoordinates(d);
	float3 sampled = gTextures[environment.textureId].SampleLevel(gSamplers[environment.samplerId], uv, lod).rgb;
	if (environment.skySunRadiance.w > 0.0 && lod < 1.0)
		sampled = lerp(SkyRadiance(environment, d), sampled, lod);
	return sampled * gPushConstants.environmentIntensity;
}

// the radiance the sheen (KHR_materials_sheen's Charlie lobe) reflects along r for a sheen roughness, scaled by the
// intensity: its directional albedo times this is the environment's sheen
float3 EnvironmentSheenRadiance(EnvironmentData environment, float3 r, float roughness)
{
	float lod = roughness * (environment.sheenLevelCount - 1.0);
	float2 uv = EnvironmentCoordinates(EnvironmentDirection(r));
	return gTextures[SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE].SampleLevel(gSamplers[environment.samplerId], uv, lod).rgb *
		   gPushConstants.environmentIntensity;
}

// the direction to look the prefiltered environment up along for a reflection r about n: the GGX lobe's peak leans from
// r toward n as the roughness grows (Frostbite's dominant direction, Lagarde and de Rousiers), which matters at grazing
// angles, where r skims the horizon but most of a rough lobe's light comes from above it
float3 DominantDirection(float3 n, float3 r, float roughness)
{
	float a = roughness * roughness;
	return normalize(lerp(n, r, (1.0 - a) * (sqrt(1.0 - a) + a)));
}
