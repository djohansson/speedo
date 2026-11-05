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

// the moon's disk along d (see EnvironmentData::skyMoon): a sphere lit by the sun on its phase, its normal at the disk's
// point facing the viewer at the middle (the moon's direction, reversed) and leaning out to its edge, Lambertian (its
// radiance by the normal's cosine to the sun), skyMoonRadiance being the disk's mean over the share of a lit sphere's
// light the phase shows, so that the disk gives the moon's illuminance; its edge soft over a fifth of its radius, as the
// sun's. not yet: the earthshine on its dark side, its maria
float3 SkyMoonDisk(EnvironmentData environment, float3 d)
{
	float3 moon = environment.skyMoon.xyz;
	float radius = acos(clamp(environment.skyMoon.w, -1.0, 1.0));
	if (radius <= 0.0 || environment.skyMoonRadiance.w <= 0.0)
		return 0.0;
	float angle = 2.0 * asin(min(0.5 * length(d - moon), 1.0));
	float disk = 1.0 - smoothstep(radius * 0.9, radius * 1.1, angle);
	if (disk <= 0.0)
		return 0.0;
	float3 across = (d - moon * dot(d, moon)) / max(sin(radius), 1e-6); // the disk's point from its middle, in radii
	float3 normal = normalize(across - moon * sqrt(saturate(1.0 - dot(across, across))));
	return environment.skyMoonRadiance.rgb * saturate(dot(normal, environment.skySun.xyz)) * disk;
}

uint SkyStarHash(uint2 cell)
{
	uint hash = (cell.x * 0x8da6b343u) ^ (cell.y * 0xd8163841u);
	hash ^= hash >> 13;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;
	return hash;
}

// the stars along d, once the sun is below the horizon (by day the sky outshines them a thousand to one): the celestial
// sphere's octahedral grid of SHADER_TYPES_SKY_STAR_CELL_COUNT cells a side (GiOctahedral, the north celestial pole at its
// middle), a cell in eighty or so holding a star (by its hash) at a point of it, of a magnitude drawn so that their count
// grows with faintness as the sky's does (three times a magnitude, from the brightest, -1.5, to the eye's faintest, 6.5:
// some twelve thousand, seven of them brighter than 0), each a Gaussian as wide as a pixel of the shading view (ViewRay
// a pixel apart) carrying the illuminance a star of its magnitude gives (2.5 microlux at magnitude 0), the 3x3 cells about
// the direction's searched so that a star near a cell's edge isn't missed (a cell past the map's edge folded back onto
// it: an edge's two halves are the same meridian, mirrored). in the equatorial frame: the direction's hour angle and
// declination by the latitude, its right ascension by the local sidereal time (skyNight.xy; the panorama's +x east, +y
// up, -z north), so the sky turns through the night and with the latitude. not yet: the real catalogue, the air dimming
// them toward the horizon, the Milky Way, the planets
float3 SkyStars(EnvironmentData environment, float3 d)
{
	if (environment.skySun.y > 0.0 || environment.skySunRadiance.w <= 0.0)
		return 0.0;
	float latitude = environment.skyNight.y;
	float3 local = float3(d.x, d.y, -d.z); // east, up, north
	float sinDeclination = local.y * sin(latitude) + local.z * cos(latitude);
	float hourAngle = atan2(-local.x, local.y * cos(latitude) - local.z * sin(latitude));
	float ascension = environment.skyNight.x - hourAngle;
	float cosDeclination = sqrt(saturate(1.0 - sinDeclination * sinDeclination));
	float3 equatorial = float3(cosDeclination * cos(ascension), cosDeclination * sin(ascension), sinDeclination);
	int cells = int(SHADER_TYPES_SKY_STAR_CELL_COUNT);
	int2 cell = int2(floor((GiOctahedral(equatorial) * 0.5 + 0.5) * float(cells)));
	ViewData view = gViewData[gPushConstants.frameIndex][min(gShadingViewId, SHADER_TYPES_SPLIT_VIEW_COUNT - 1u)];
	float pixelAngle = max(length(ViewRay(view, float2(0.0, 2.0 / max(view.viewport.w, 1.0))) - ViewRay(view, float2(0.0, 0.0))), 1e-5);
	float sigma = 0.7 * pixelAngle;
	float3 light = 0.0;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			int2 other = cell + int2(x, y);
			if (other.x < 0)
				other = int2(0, cells - 1 - other.y);
			else if (other.x >= cells)
				other = int2(cells - 1, cells - 1 - other.y);
			if (other.y < 0)
				other = int2(cells - 1 - other.x, 0);
			else if (other.y >= cells)
				other = int2(cells - 1 - other.x, cells - 1);
			uint hash = SkyStarHash(uint2(other));
			if ((hash & 0xffu) >= 3u)
				continue;
			float2 point = (float2(other) + float2(float((hash >> 8) & 0xffu), float((hash >> 16) & 0xffu)) / 255.0) / float(cells) * 2.0 - 1.0;
			float3 star = GiFromOctahedral(point);
			float u = (float(SkyStarHash(uint2(hash, 0x9e3779b9u))) + 0.5) / 4294967296.0;
			float magnitude = max(6.5 + 2.0 * log10(u), -1.5);
			float illuminance = 2.5e-6 * SHADER_TYPES_ATMOSPHERE_LUX * pow(10.0, -0.4 * magnitude);
			float angle = length(star - equatorial);
			light += illuminance / (2.0 * kPi * sigma * sigma) * exp(-0.5 * angle * angle / (sigma * sigma));
		}
	return light;
}

// the sky's radiance in a direction (in the panorama's space), sun included (see EnvironmentData::skySun): its view of
// itself (see sky.h), and the sun's disk through the atmosphere, its edge soft over a fifth of its radius (a pixel or two;
// the angle as twice the asin of half the chord, which keeps its precision where acos of the cosine loses it); then the
// night's: the moon's sky (from the ground), the night's glow, the moon's disk on its phase and the stars
float3 SkyRadiance(EnvironmentData environment, float3 d)
{
	// from the shading view's eye at its altitude, where the sky's views from the eyes are made (ComputeSkyEyeView), else
	// from the ground (the environment's own table, half a metre up, which lights the scene)
	uint frameIndex = gPushConstants.frameIndex;
	AerialPerspectiveData aerial = gAerialPerspectiveData[frameIndex][0];
	float3 color;
	if (aerial.skyEyeView != 0u && gShadingViewId < SHADER_TYPES_SPLIT_VIEW_COUNT)
	{
		float r = SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS + AerialPerspectiveAltitude(aerial, gViewData[frameIndex][gShadingViewId].eyePosition.y);
		color = SkyViewRadianceFrom(
			gTextures[SHADER_TYPES_SKY_EYE_VIEW_TEXTURE], gSamplers[environment.samplerId], d, environment.skySun.xyz, r, gShadingViewId,
			SHADER_TYPES_SPLIT_VIEW_COUNT);
	}
	else
		color = SkyViewRadiance(gTextures[SHADER_TYPES_SKY_VIEW_TEXTURE], gSamplers[environment.samplerId], d, environment.skySun.xyz);
	float radius = acos(environment.skySun.w);
	float angle = 2.0 * asin(min(0.5 * length(d - environment.skySun.xyz), 1.0));
	color += environment.skySunRadiance.rgb * (1.0 - smoothstep(radius * 0.9, radius * 1.1, angle)) + environment.skyNight.z;
	if (environment.skyMoonRadiance.w > 0.0)
		color += SkyViewRadiance(gTextures[SHADER_TYPES_SKY_MOON_VIEW_TEXTURE], gSamplers[environment.samplerId], d, environment.skyMoon.xyz) +
				 SkyMoonDisk(environment, d);
	return color + SkyStars(environment, d);
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
