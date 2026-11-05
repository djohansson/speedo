// the aerial perspective (see AerialPerspectiveData): the light the sky's atmosphere scatters toward the eye between it and
// what it sees, and what it lets through of that, as Hillaire 2020's aerial perspective volume (the sky's own tables and
// medium, see sky.h, so that what is drawn fades into the sky as the sky's horizon has it), lit by the sky's sun through its
// shadow views (light shafts): ComputeAerialPerspective makes it, AerialPerspectiveApply applies it. a part of
// shaders.slang, which includes it after capi.h and sky.h

// the slice (fractional, 0 at the eye) of a distance from it in meters, and back: SHADER_TYPES_AERIAL_PERSPECTIVE_NEAR at
// slice 1, then a constant factor farther each slice, to _FAR at the last (past it, the last)
float AerialPerspectiveSlice(float meters)
{
	float near = SHADER_TYPES_AERIAL_PERSPECTIVE_NEAR;
	float last = float(SHADER_TYPES_AERIAL_PERSPECTIVE_SLICE_COUNT - 1u);
	if (meters <= near)
		return max(meters, 0.0) / near;
	return min(1.0 + log2(meters / near) / log2(SHADER_TYPES_AERIAL_PERSPECTIVE_FAR / near) * (last - 1.0), last);
}

float AerialPerspectiveDistance(float slice)
{
	float near = SHADER_TYPES_AERIAL_PERSPECTIVE_NEAR;
	float last = float(SHADER_TYPES_AERIAL_PERSPECTIVE_SLICE_COUNT - 1u);
	if (slice <= 1.0)
		return slice * near;
	return near * exp2((slice - 1.0) / (last - 1.0) * log2(SHADER_TYPES_AERIAL_PERSPECTIVE_FAR / near));
}

// the altitude (km over the planet's surface) of a height in the scene (its y): the scene's lowest point is on the surface,
// where an eye is SHADER_TYPES_ATMOSPHERE_EYE_ALTITUDE up, the sky's (see AerialPerspectiveData::groundHeight)
float AerialPerspectiveAltitude(AerialPerspectiveData data, float y)
{
	return SHADER_TYPES_ATMOSPHERE_EYE_ALTITUDE + max(y - data.groundHeight, 0.0) * data.metersPerUnit * 1e-3;
}

// the fog near the ground (see AerialPerspectiveData::fog): its extinction at an altitude (km), per km
float FogExtinction(AerialPerspectiveData data, float altitude)
{
	return data.fog.x * exp(-max(altitude, 0.0) / data.fog.y);
}

// its optical depth from an altitude (km) to the sky, along a direction rising at a sine of rise: the layer is level over
// the ground it covers, and a ray along the ground leaves it where the planet's curve has taken it up the scale height,
// sqrt(2 R H) away (the least rise, sqrt(2 H / (pi R)), gives that depth: Chapman's at the horizon)
float FogDepthToSky(AerialPerspectiveData data, float altitude, float rise)
{
	float height = data.fog.y;
	float least = sqrt(2.0 * height / (kSkyPi * SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS));
	return data.fog.x * height * exp(-max(altitude, 0.0) / height) / max(rise, least);
}

// its optical depth from an altitude (km) down to the surface, along a direction falling at a sine of fall: the layer
// below the eye (its density's mean over it times the way through it), the way no longer than the surface's horizon from
// the eye, sqrt(2 R h), where a ray along the ground meets it (the environment's ground is the surface: a ray toward it
// used to take the whole layer above the eye, as toward the sky)
float FogDepthToGround(AerialPerspectiveData data, float altitude, float fall)
{
	float height = data.fog.y;
	float above = max(altitude, 1e-6);
	float way = above / max(fall, above / sqrt(2.0 * SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS * above));
	return data.fog.x * height * (1.0 - exp(-above / height)) / above * way;
}

// its optical depth along a straight segment of a length (km) from an altitude to another (km): the mean of its density
// between them, which falls exponentially with the height, times the length
float FogDepthAlong(AerialPerspectiveData data, float from, float to, float length)
{
	float height = data.fog.y;
	float low = max(from, 0.0);
	float high = max(to, 0.0);
	float a = exp(-low / height);
	float b = exp(-high / height);
	float mean = abs(high - low) > 1e-3 * height ? (a - b) * height / (high - low) : 0.5 * (a + b);
	return data.fog.x * mean * length;
}

// how much of the sun's light reaches a point in the air (in the world), by the light's shadow views (a directional
// light's cascades, finest first, as PlaceInShadowMap takes them, without its normal offset: the air has no surface): a
// bilinear comparison in the finest that holds it, lit where none does (past the scene, whose casters they hold)
float AerialPerspectiveShadow(uint lightIndex, float3 position)
{
	uint frameIndex = gPushConstants.frameIndex;
	uint first = gLightShadows[frameIndex][lightIndex];
	if (first == SHADER_TYPES_NO_SHADOW)
		return 1.0;
	Texture2D atlas = gTextures[SHADER_TYPES_SHADOW_ATLAS_TEXTURE];
	float2 atlasSize;
	atlas.GetDimensions(atlasSize.x, atlasSize.y);
	uint viewCount = SHADER_TYPES_SHADOW_CASCADE_COUNT * max(gPushConstants.mainViewCount, 1u);
	for (uint cascade = 0; cascade < viewCount; cascade++)
	{
		ShadowData shadow = gShadowViews[frameIndex][first + cascade];
		float4 clip = mul(shadow.viewProjection, float4(position, 1.0));
		if (clip.w <= 0.0)
			continue;
		float3 ndc = clip.xyz / clip.w;
		float2 local = ndc.xy * 0.5 + 0.5;
		// the comparison's texels inside the tile
		float2 margin = 1.0 / (atlasSize * shadow.atlasRect.zw);
		if (any(local < margin) || any(local > 1.0 - margin) || ndc.z < 0.0)
			continue; // past its far plane (depth reversed: 0 there), or outside it
		if (ndc.z > 1.0)
			return 1.0; // nearer the light than anything it holds
		float2 st = (shadow.atlasRect.xy + local * shadow.atlasRect.zw) * atlasSize - 0.5;
		float2 base = floor(st);
		float2 f = st - base;
		// the four texels around: x (0, 1), y (1, 1), z (1, 0), w (0, 0)
		float4 stored = atlas.GatherRed(gSamplers[SHADER_TYPES_CLAMP_SAMPLER], (base + 1.0) / atlasSize);
		float4 visible = select(stored <= ndc.z, float4(1.0), float4(0.0)); // depth reversed: farther is smaller
		return lerp(lerp(visible.w, visible.z, f.x), lerp(visible.x, visible.y, f.x), f.y);
	}
	return 1.0;
}

// the light around a point in the air (rgb: its radiance's mean over the sphere, which the air scatters toward the eye by
// its isotropic share, as the multiple scattering's light), from the indirect lighting's probes rather than the open sky's:
// what a roofed corridor's air has is the dim light within it, and what its sunlit floor throws back. the 8 probes around
// it in each clip that holds it (blended as GiSampleAt blends them, by GiClipWeight), trilinear, each the mean of its
// faces (irradiance over pi along the axes: the mean radiance, for light from all around), those that traced and aren't
// inside something alone (the volumes are only resolved near surfaces: the lookups are at them; the probes, within their
// cell of one); a: how much of the point the probes hold (0 outside the clips, and where none of them traced: open air,
// far from any surface, which the sky's light then takes)
float4 AerialPerspectiveAmbient(float3 position)
{
	uint frameIndex = gPushConstants.frameIndex;
	if (gGi[frameIndex][0].enabled == 0)
		return 0.0;
	uint clipCount = gGi[frameIndex][0].clipCount;
	float3 light = 0.0;
	float held = 0.0;
	float remaining = 1.0;
	for (uint clipIt = 0; clipIt < clipCount && remaining > 0.0; clipIt++)
	{
		float weight = GiClipWeight(GiClipViewAt(clipIt), position);
		if (weight <= 0.0)
			continue;
		GiData gi = GiClip(clipIt);
		float3 fraction;
		int3 base = GiProbeBase(gi, position, fraction);
		float3 sum = 0.0;
		float sumWeight = 0.0;
		for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		{
			int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
			int3 probe = base + offset;
			if (!GiInside(probe, gi.probeSize.xyz))
				continue;
			uint first = (gi.probeFirst + GiProbeIndex(uint3(probe), gi.probeSize)) * SHADER_TYPES_GI_IRRADIANCE_COUNT;
			float4 face0 = gGiIrradiance[first];
			float4 face5 = gGiIrradiance[first + 5];
			if (face0.w <= 0.0 || face5.w > kGiInsideShare)
				continue; // traced nothing (see ComputeGiProbeUpdate), or inside something
			float3 weights = select(offset != 0, fraction, 1.0 - fraction);
			float corner = weights.x * weights.y * weights.z;
			float3 faces = face0.rgb + gGiIrradiance[first + 1].rgb + gGiIrradiance[first + 2].rgb + gGiIrradiance[first + 3].rgb +
						   gGiIrradiance[first + 4].rgb + face5.rgb;
			sum += corner * max(faces, 0.0) / 6.0;
			sumWeight += corner;
		}
		// a clip whose probes there traced nothing leaves the point to the coarser ones
		if (sumWeight <= 1e-3)
			continue;
		float share = remaining * weight;
		remaining -= share;
		light += share * sum / sumWeight;
		held += share;
	}
	return float4(light, held);
}

// the light around the fog near the ground under an open sky along a direction, which it scatters by its phase: mostly
// forward, so mostly what comes from behind it that way, the environment's radiance along the direction (the sky's
// without its sun, a directional light, over the ground lit by it; its second prefiltered level, roughness 0.2, which
// resolves the horizon's bright band: prefiltered to about the phase's width, roughness 0.5, the band was blurred into
// the darker sky above it and the ground below, and the fog's horizon came out at half the clear sky's, a dark band over
// the ground plane's horizon; a thin layer's light scattered twice or more, which the single scattering here leaves out,
// is what makes a real haze's horizon as bright as the clear sky's or brighter), for three quarters (the phase's weight
// within 60 degrees of forward, at SHADER_TYPES_FOG_G), and the sky's and the ground's mean for the rest (the
// environment's irradiance over pi, its mean over the sphere: the spherical harmonics' first coefficient, the others
// averaging to nothing over it). not the air's multiple scattering alone, which the clear air takes (the fog isn't in
// the sky's tables, so the sky's whole light is light the fog scatters again): by the table alone, or the mean alone, a
// fog along the ground darkened the horizon a tenth to a third below the clear sky's, a gray line between the sky and
// the lit ground, the air's phase scattering a high sun sideways four times as much as the fog's
float3 FogAround(EnvironmentData environment, float3 direction)
{
	const float kForwardShare = 0.75;
	const float kForwardRoughness = 0.2;
	float3 mean = max(environment.irradiance[0].rgb * 0.282095, 0.0) * gPushConstants.environmentIntensity;
	return kForwardShare * EnvironmentRadiance(environment, direction, kForwardRoughness) + (1.0 - kForwardShare) * mean;
}

// the volume (gAerialPerspectiveImages): a thread per cell of a view's viewport (z the view), marching its ray from the eye
// through the atmosphere (the eye at its altitude over the planet's surface, see AerialPerspectiveData::groundHeight; the
// planet's center straight below it, +y up) slice by slice, each in SHADER_TYPES_AERIAL_PERSPECTIVE_STEP_COUNT steps,
// integrated as the sky's (SkyScatteredLight: the source constant over a step, the transmittance exponential): the sun's
// single scattering by each phase function, shadowed by its shadow views where it casts light shafts, and the light
// scattered more than once: the indirect lighting's around the air where its probes hold it (AerialPerspectiveAmbient),
// else the sky's multiple scattering (as under an open sky: in a roofed corridor, the air glowed with skylight it has
// none of). in the renderer's units. the steps are offset per cell by interleaved gradient noise, the same every frame,
// so that a shaft's edge dithers across cells rather than banding along slices; the volume's slices store what reaches
// the eye from up to each, as far as the planet's surface where the ray meets it (groundReach). the fog near the ground
// (AerialPerspectiveData::fog) is part of the air, and the sun's light reaching a point comes through it (FogDepthToSky);
// the backdrop takes the fog in closed form (FogToSky)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeAerialPerspective(uint3 id : SV_DispatchThreadID)
{
	uint frameIndex = gPushConstants.frameIndex;
	AerialPerspectiveData data = gAerialPerspectiveData[frameIndex][0];
	if (id.x >= SHADER_TYPES_AERIAL_PERSPECTIVE_WIDTH || id.y >= SHADER_TYPES_AERIAL_PERSPECTIVE_HEIGHT || id.z >= data.viewCount)
		return;

	ViewData view = gViewData[frameIndex][id.z];
	float2 ndc = (float2(id.xy) + 0.5) / float2(SHADER_TYPES_AERIAL_PERSPECTIVE_WIDTH, SHADER_TYPES_AERIAL_PERSPECTIVE_HEIGHT) * 2.0 - 1.0;
	float3 ray = ViewRay(view, ndc);
	float3 eye = view.eyePosition.xyz;

	float metersPerUnit = data.metersPerUnit;
	float3 origin = float3(0.0, SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS + AerialPerspectiveAltitude(data, eye.y), 0.0); // km
	bool fog = data.fog.x > 0.0;
	float3 sun = data.sun.xyz;
	float cosine = dot(ray, sun);
	float rayleighPhase = SkyRayleighPhase(cosine);
	float miePhase = SkyMiePhase(cosine);
	float fogPhase = SkyCornetteShanksPhase(cosine, SHADER_TYPES_FOG_G);
	Texture2D transmittanceTable = gTextures[SHADER_TYPES_SKY_TRANSMITTANCE_TEXTURE];
	Texture2D multipleTable = gTextures[SHADER_TYPES_SKY_MULTIPLE_SCATTERING_TEXTURE];
	SamplerState linear = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	float aerosols = gEnvironment[0].skyAtmosphere.x; // as the sky's tables were made for
	bool shafts = data.shaftLight != SHADER_TYPES_NO_SHADOW;
	// the light around the fog under an open sky (see FogAround)
	float3 skyAround = fog ? FogAround(gEnvironment[0], ray) : 0.0;
	// the steps' offset within a slice: per cell by interleaved gradient noise, so that a shaft's edge dithers across
	// cells rather than banding along slices, and turned every frame by the golden ratio, so that the frames' samples
	// differ, which the history averages (the same every frame, the steps aliased against the shadows' edges in dense fog,
	// dotted arcs across FogHall's sheets); the slices past the history take the middle
	float noise = frac(52.9829189 * frac(dot(float2(id.xy) + float(id.z) * 17.0, float2(0.06711056, 0.00583715))));
	float offset = frac(noise + float(data.frame % 64u) * 0.6180339887);
	// the scene's reach along the ray, in meters (the view's depth over the ray's share of it): past it, no shadow view
	// nor probe holds anything
	float3 forward = ViewRay(view, float2(0.0, 0.0));
	float sceneReach = data.sceneDepths[min(id.z, 3u)] / max(dot(ray, forward), 1e-3) * metersPerUnit;
	// where the ray meets the planet's surface (meters; none where it doesn't): the march ends there, the slices past it
	// holding what reached the eye from the surface. marched on through the planet, a cell's ray below the horizon crossed
	// air of the surface's density for 200 km, denser still by the exponential layers' negative altitudes, with no sun
	// (the planet is in the way of every point under its surface): a saturated, dark fog, which the sky's last rows took
	// bilinearly from the cells below the horizon, a dark band over the ground plane's horizon, and the plane's own last
	// rows, farther than the surface, took whole
	float groundDistance = SkySphere(origin, ray, SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS);
	float groundReach = groundDistance >= 0.0 ? groundDistance * 1e3 : SHADER_TYPES_AERIAL_PERSPECTIVE_FAR * 2.0;
	// the history (gAerialPerspectiveHistory): each of the first SHADER_TYPES_AERIAL_PERSPECTIVE_HISTORY_SLICE_COUNT
	// slices' light per meter and the fog's share of it, from where its middle was in the last frame's view (the last view
	// projection and eye, where the view was at this place then), blended in by historyBlend; per meter, since the slices
	// there are of other lengths; none for a slice the last frame's volume doesn't hold (behind its eye, outside its view,
	// past its history)
	uint first = id.z * SHADER_TYPES_AERIAL_PERSPECTIVE_SLICE_COUNT;
	uint historyFirst = id.z * SHADER_TYPES_AERIAL_PERSPECTIVE_HISTORY_SLICE_COUNT;
	float historyDepth = float(SHADER_TYPES_AERIAL_PERSPECTIVE_HISTORY_SLICE_COUNT * SHADER_TYPES_SPLIT_VIEW_COUNT);
	bool viewHeld = data.history != 0u && view.previousEyePosition.w > 0.0;
	Texture3D<float4> history = gAerialPerspectiveHistory[data.historySlot ^ 1u];
	RWTexture3D<float4> historyImage = gAerialPerspectiveHistoryImages[data.historySlot];

	gAerialPerspectiveImages[0][uint3(id.xy, first)] = float4(0.0, 0.0, 0.0, 0.0);
	gAerialPerspectiveImages[1][uint3(id.xy, first)] = float4(1.0, 1.0, 1.0, 1.0);
	float3 light = 0.0; // in the renderer's units
	float3 transmittance = 1.0;
	float previous = 0.0; // meters
	float4 ambient = 0.0;
	for (uint slice = 1; slice < SHADER_TYPES_AERIAL_PERSPECTIVE_SLICE_COUNT; slice++)
	{
		float distance = AerialPerspectiveDistance(float(slice));
		float sliceLength = distance - previous;
		float step = sliceLength / float(SHADER_TYPES_AERIAL_PERSPECTIVE_STEP_COUNT);
		bool held = slice <= SHADER_TYPES_AERIAL_PERSPECTIVE_HISTORY_SLICE_COUNT;
		float stepOffset = held ? offset : 0.5;
		// the light around the air in the scene, the indirect lighting's at the slice's middle (it changes slowly: taken
		// every other slice, for two, the 8 probes' 6 faces each being most of a slice's reads), where its probes hold it
		float middle = 0.5 * (previous + distance);
		if ((slice & 1u) != 0u)
			ambient = middle <= sceneReach ? AerialPerspectiveAmbient(eye + ray * (middle / metersPerUnit)) : 0.0;
		// the slice's own: what it scatters toward its start, and what it lets through
		float3 sliceLight = 0.0;
		float3 sliceTransmittance = 1.0;
		for (uint stepIt = 0; stepIt < SHADER_TYPES_AERIAL_PERSPECTIVE_STEP_COUNT; stepIt++)
		{
			// the step, cut where the ray meets the surface (meters)
			float stepStart = previous + float(stepIt) * step;
			float stepEnd = min(stepStart + step, groundReach);
			if (stepEnd <= stepStart)
				break;
			float stepLength = (stepEnd - stepStart) * 1e-3; // km
			float at = lerp(stepStart, stepEnd, stepOffset);
			float3 position = origin + ray * (at * 1e-3);
			float r = length(position);
			float muSun = dot(position / r, sun);
			float altitude = r - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS;
			// the medium over the step, its exponential layers' mean between the altitudes of the step's ends
			// (SkyMediumAlong, FogDepthAlong): taken at a point, a far step kilometres long from above the fog, crossing
			// the layer where it lands, had its density there, another each row of cells, stripes across the sky at the
			// horizon from 500 m up
			float altitudeStart = length(origin + ray * (stepStart * 1e-3)) - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS;
			float altitudeEnd = length(origin + ray * (stepEnd * 1e-3)) - SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS;
			SkyMedium medium = SkyMediumAlong(altitudeStart, altitudeEnd, aerosols);
			// the fog: it scatters what it takes (water and haze), by a phase of its own (SHADER_TYPES_FOG_G)
			float fogExtinction = fog ? FogDepthAlong(data, altitudeStart, altitudeEnd, stepLength) / stepLength : 0.0;
			float3 extinction = medium.extinction + fogExtinction;
			float3 sunLight = SkyTransmittance(transmittanceTable, linear, r, muSun) * SkySunVisible(position, sun);
			if (fog)
				sunLight *= exp(-FogDepthToSky(data, altitude, muSun));
			if (shafts && at <= sceneReach && any(sunLight > 0.0))
				sunLight *= AerialPerspectiveShadow(data.shaftLight, eye + ray * (at / metersPerUnit));
			// the light scattered more than once: the sky's, as under an open sky, where the probes don't hold the air (the
			// fog's the sky's whole light, see skyAround)
			float3 around = ambient.rgb + (1.0 - ambient.a) * SkyMultipleScattering(multipleTable, linear, r, muSun) * data.sun.w;
			float3 fogAround = ambient.rgb + (1.0 - ambient.a) * skyAround;
			float3 source = sunLight * (medium.rayleigh * rayleighPhase + medium.mie * miePhase + fogExtinction * fogPhase) * data.sun.w +
							around * medium.scattering + fogAround * fogExtinction;
			float3 through = exp(-extinction * stepLength);
			sliceLight += sliceTransmittance * (source - source * through) / max(extinction, 1e-9);
			sliceTransmittance *= through;
		}
		if (held)
		{
			float4 stored = float4(sliceLight / sliceLength, 0.0);
			if (viewHeld)
			{
				float3 point = eye + ray * (middle / metersPerUnit);
				float4 clip = mul(view.previousViewProjection, float4(point, 1.0));
				if (clip.w > 0.0)
				{
					float2 uv = clip.xy / clip.w * 0.5 + 0.5;
					float previousSlice = AerialPerspectiveSlice(length(point - view.previousEyePosition.xyz) * metersPerUnit);
					if (all(uv >= 0.0) && all(uv <= 1.0) && previousSlice >= 1.0 && previousSlice <= float(SHADER_TYPES_AERIAL_PERSPECTIVE_HISTORY_SLICE_COUNT))
					{
						float3 uvw = float3(uv, (float(historyFirst) + previousSlice - 1.0 + 0.5) / historyDepth);
						stored = lerp(stored, history.SampleLevel(linear, uvw, 0.0), data.historyBlend);
					}
				}
			}
			historyImage[uint3(id.xy, historyFirst + slice - 1u)] = stored;
			sliceLight = stored.rgb * sliceLength;
		}
		light += transmittance * sliceLight;
		transmittance *= sliceTransmittance;
		previous = distance;
		gAerialPerspectiveImages[0][uint3(id.xy, first + slice)] = float4(light, 0.0);
		gAerialPerspectiveImages[1][uint3(id.xy, first + slice)] = float4(transmittance, 1.0);
	}
}

// the sky's view of itself from each split view's eye (SHADER_TYPES_SKY_EYE_VIEW_RW_TEXTURE, the views' tables stacked
// along y): as the environment's table (EnvironmentSkyView, from half a metre up, which lights the scene), but from the
// eye's altitude over the planet's surface (AerialPerspectiveAltitude), so that from high over the ground the sky's
// horizon dips as the ground's does, with the air below it as the aerial perspective has it over the ground (by the
// ground's table the sky's horizon stayed where the surface's is, above the air's); a thread per texel and view, of the
// views AerialPerspectiveData::skyEyeViewRedraw says (the eye's altitude, the sun or the air changed), in the renderer's
// units. the sun's disk is the environment's, from the ground (SkyRadiance)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeSkyEyeView(uint3 id : SV_DispatchThreadID)
{
	uint frameIndex = gPushConstants.frameIndex;
	AerialPerspectiveData data = gAerialPerspectiveData[frameIndex][0];
	if (id.x >= SHADER_TYPES_SKY_VIEW_WIDTH || id.y >= SHADER_TYPES_SKY_VIEW_HEIGHT || id.z >= data.viewCount ||
		(data.skyEyeViewRedraw & (1u << id.z)) == 0u)
		return;
	float2 uv = (float2(id.xy) + 0.5) / float2(SHADER_TYPES_SKY_VIEW_WIDTH, SHADER_TYPES_SKY_VIEW_HEIGHT);
	float r = SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS + AerialPerspectiveAltitude(data, gViewData[frameIndex][id.z].eyePosition.y);
	float viewZenithCos;
	float sunViewCos;
	SkyViewParametersFrom(uv, r, viewZenithCos, sunViewCos);
	float muSun = clamp(gEnvironment[0].skySun.y, -1.0, 1.0);
	float3 sunDirection = float3(sqrt(max(1.0 - muSun * muSun, 0.0)), muSun, 0.0);
	float sinZenith = sqrt(max(1.0 - viewZenithCos * viewZenithCos, 0.0));
	float3 direction = float3(sinZenith * sunViewCos, viewZenithCos, sinZenith * sqrt(max(1.0 - sunViewCos * sunViewCos, 0.0)));
	float3 light = SkyScatteredLight(
		gTextures[SHADER_TYPES_SKY_TRANSMITTANCE_TEXTURE], gTextures[SHADER_TYPES_SKY_MULTIPLE_SCATTERING_TEXTURE], gSamplers[SHADER_TYPES_CLAMP_SAMPLER],
		float3(0.0, r, 0.0), direction, sunDirection, 32, gEnvironment[0].skyAtmosphere.x);
	gRWTextures[SHADER_TYPES_SKY_EYE_VIEW_RW_TEXTURE][uint2(id.x, id.z * SHADER_TYPES_SKY_VIEW_HEIGHT + id.y)] =
		float4(light * SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE, 1.0);
}

// the aerial perspective at what a pixel sees a distance from the eye (in the scene's units): at local, its place in its
// view's viewport (0 to 1), of the view index'th volume. light: what the air scatters toward the eye up to it, transmittance:
// what it lets through of it
void AerialPerspectiveAt(AerialPerspectiveData data, float2 local, uint viewIndex, float distance, out float3 light, out float3 transmittance)
{
	float slice = AerialPerspectiveSlice(distance * data.metersPerUnit);
	float depth = float(SHADER_TYPES_AERIAL_PERSPECTIVE_SLICE_COUNT * SHADER_TYPES_SPLIT_VIEW_COUNT);
	float3 uvw = float3(saturate(local), (float(viewIndex * SHADER_TYPES_AERIAL_PERSPECTIVE_SLICE_COUNT) + slice + 0.5) / depth);
	SamplerState linear = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	light = gAerialPerspective[0].SampleLevel(linear, uvw, 0.0).rgb;
	transmittance = gAerialPerspective[1].SampleLevel(linear, uvw, 0.0).rgb;
}

// what the fog near the ground does to the environment seen along a direction from a height y in the scene, in closed
// form (no shadows): what it lets through, exp of the optical depth through the layer that way (FogDepthToSky, rising;
// FogDepthToGround, falling: the environment's ground is the surface), and what it scatters toward the eye, its source
// (the sun's light through the layer above by the fog's phase, and the light around, FogAround) over the depth, the
// source constant along the way. for the backdrop, the sky a ray or a refraction shows, and the environment the shading
// reflects (the sky has the rest of the air already); nothing where the air isn't drawn. the backdrop took the volume's
// slice of the fog alone along the whole ray until 2026-11-05: its cells, 16 pixels apart, straddle the horizon, and the
// sky's last rows took half of the cells below it (through the planet then, see ComputeAerialPerspective), a dark band
// over the ground plane's horizon; per pixel the fog's horizon is a line, as the sky's
void FogToSky(AerialPerspectiveData data, float3 direction, float y, out float3 light, out float3 transmittance)
{
	light = 0.0;
	transmittance = 1.0;
	if (data.enabled == 0u || data.fog.x <= 0.0)
		return;
	float altitude = AerialPerspectiveAltitude(data, y);
	float depth = direction.y >= 0.0 ? FogDepthToSky(data, altitude, direction.y) : FogDepthToGround(data, altitude, -direction.y);
	float through = exp(-depth);
	float3 sun = data.sun.xyz;
	float r = SHADER_TYPES_ATMOSPHERE_BOTTOM_RADIUS + altitude; // the planet's center straight below: the vertical is +y
	float3 sunLight = SkyTransmittance(gTextures[SHADER_TYPES_SKY_TRANSMITTANCE_TEXTURE], gSamplers[SHADER_TYPES_CLAMP_SAMPLER], r, sun.y) *
					  SkySunVisible(float3(0.0, r, 0.0), sun) * exp(-FogDepthToSky(data, altitude, sun.y));
	float3 source = sunLight * SkyCornetteShanksPhase(dot(direction, sun), SHADER_TYPES_FOG_G) * data.sun.w + FogAround(gEnvironment[0], direction);
	light = source * (1.0 - through);
	transmittance = float3(through);
}

// the air between two distances from the eye (the scene's units) along the shading pixel's ray (gFragmentPixel, in
// gShadingViewId's view), by the volume: what it lets through of what is at the farther and what it adds, from the
// integrals from the eye (the farther's light is the nearer's plus the nearer's transmittance times the air's between).
// for what a ray from a surface shows, as if that lay along the eye's ray that much farther: the air is nearly the same
// within the scene's reach, the shadows and the probes aside. nothing where the air isn't drawn
void AerialPerspectiveBetween(float near, float far, out float3 light, out float3 transmittance)
{
	light = 0.0;
	transmittance = 1.0;
	uint frameIndex = gPushConstants.frameIndex;
	AerialPerspectiveData data = gAerialPerspectiveData[frameIndex][0];
	if (data.enabled == 0u)
		return;
	ViewData view = gViewData[frameIndex][gShadingViewId];
	float2 local = (float2(gFragmentPixel) + 0.5 - view.viewport.xy) / view.viewport.zw;
	float3 nearLight;
	float3 nearTransmittance;
	float3 farLight;
	float3 farTransmittance;
	AerialPerspectiveAt(data, local, gShadingViewId, near, nearLight, nearTransmittance);
	AerialPerspectiveAt(data, local, gShadingViewId, far, farLight, farTransmittance);
	transmittance = farTransmittance / max(nearTransmittance, 1e-6);
	light = max(farLight - nearLight, 0.0) / max(nearTransmittance, 1e-6);
}

// the same at what a pixel sees at a depth (reversed, the main depth's): a surface's color is then its color times
// transmittance, plus light (where something was drawn: the backdrop, the sky, has it already)
void AerialPerspectiveAtDepth(
	AerialPerspectiveData data, ViewData view, uint viewIndex, uint2 pixel, float depth, out float3 light, out float3 transmittance)
{
	float2 local = (float2(pixel) + 0.5 - view.viewport.xy) / view.viewport.zw;
	float4 world = mul(view.inverseViewProjection, float4(local * 2.0 - 1.0, depth, 1.0));
	float distance = length(world.xyz / world.w - view.eyePosition.xyz);
	AerialPerspectiveAt(data, local, viewIndex, distance, light, transmittance);
}
