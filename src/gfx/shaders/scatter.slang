// KHR_materials_volume_scatter, as screen space subsurface scattering (after the Khronos sample renderer, but by the
// extension's definitions): the light scattering in a volume is diffused under its surface. the scatter pass draws the
// light entering it (ScatterLight: from the front, which comes back out in the multiscatter color, the albedo of the
// whole of its scattering, and from behind, through the volume's thickness, measured from the back faces' depth), which
// the main pass blurs over the surface with Christensen and Burley's diffusion profile (SubsurfaceScattering), scaled per
// color by the volume's mean free path (KHR_materials_volume's attenuation is the extinction) and the albedo. the Khronos
// renderer tints by the single scattering albedo instead (what a path tracer's volume needs for the multiscatter color to
// come out, much paler), and scales the profile by the attenuation distance times the multiscatter color (a tenth of
// this), which left it a flat, pale, barely blurred diffuse.
// a part of shaders.slang, which includes it after capi.h and sky.h

// a scattering material's multiscatter color at a point: the factor times the texture
float3 MultiscatterColor(MaterialData material, float4 texCoord01)
{
	float3 multiscatter = material.multiscatterColor.rgb;
	if ((material.flags & MATERIAL_FLAG_VOLUME_SCATTER_TEXTURE) != 0u)
		multiscatter *= SampleView(material.multiscatterColorView, texCoord01).rgb;
	return multiscatter;
}

// the diffuse light coming out of a scattering surface: what enters it from the front, in the albedo (the multiscatter
// color; the environment's by occlusion), and what comes through the volume from behind (throughVolume), less what the
// specular (or sheen) reflects, times the diffuse transmission and its color
float3 ScatterLight(Surface surface, float3 view, float3 position, float3 albedo, float3 throughVolume)
{
	float3 normal = surface.normal;
	float nDotV = max(dot(normal, view), 1e-4);
	float sheenMax = max(surface.sheenColor.r, max(surface.sheenColor.g, surface.sheenColor.b));
	float sheenAlbedoView = SheenAlbedo(nDotV, surface.sheenRoughness);
	float3 behind = throughVolume;

	EnvironmentData environment = gEnvironment[0];
	float3 result = (surface.occlusion * EnvironmentIrradiance(environment, normal) * albedo + EnvironmentIrradiance(environment, -normal) * behind) *
					(1.0 - EnvironmentBrdfApprox(surface.f0, surface.f90, surface.roughness, nDotV)) * (1.0 - sheenMax * sheenAlbedoView);

	uint clusterFirst;
	uint clusterCount;
	bool clustered = ClusterLights(position, clusterFirst, clusterCount);
	for (uint clusterIt = 0; clusterIt < clusterCount; clusterIt++)
	{
		uint lightIt = clustered ? gClusterLights[gPushConstants.frameIndex][clusterFirst + clusterIt] : clusterIt;
		LightData light = gLights[lightIt];
		float3 toLight;
		float attenuation = LightAttenuation(lightIt, light, position, surface.geometryNormal, toLight);
		if (attenuation <= 0.0)
			continue;

		// from behind: the specular it isn't reflected by is the light's mirrored about the surface
		float nDotL = dot(normal, toLight);
		float3 lit = nDotL >= 0.0 ? nDotL * albedo : -nDotL * behind;
		float3 halfway = normalize((nDotL >= 0.0 ? toLight : toLight - 2.0 * nDotL * normal) + view);
		lit *= 1.0 - FresnelSchlick(surface.f0, surface.f90, saturate(dot(view, halfway)));
		if (sheenMax > 0.0)
			lit *= min(1.0 - sheenMax * sheenAlbedoView, 1.0 - sheenMax * SheenAlbedo(abs(nDotL), surface.sheenRoughness));
		result += lit / kPi * light.intensity.rgb * attenuation;
	}

	return result * surface.diffuseTransmission * surface.diffuseTransmissionColor;
}

// Christensen and Burley's diffusion profile: its scale for a mean free path and an albedo (the searchlight fit), and its
// value at a distance (per unit of area, as the samples' pdf)
float3 BurleyScale(float3 meanFreePath, float3 albedo)
{
	float3 offset = albedo - 0.8;
	return meanFreePath / (1.9 - albedo + 3.5 * offset * offset);
}

float3 BurleyEval(float3 d, float r)
{
	float3 e = exp(-r / (3.0 * d));
	return (e * e * e + e) / (4.0 * d);
}

// the profile's samples (the sample renderer's: golden angle spiral, radii importance sampled) for a scale of
// kScatterSampleScale, the offset in xy and the reciprocal of its pdf in z. the nearest is kScatterMinRadius out
static const uint kScatterSampleCount = 55;
static const float kScatterSampleScale = 0.25 / kPi / 1.04;
static const float kScatterMinRadius = 0.001395;
static const float3 kScatterSamples[kScatterSampleCount] = {
	float3(-0.001395, 0.000000, 0.969751), float3(0.003123, -0.002861, 0.993903), float3(-0.000625, 0.007120, 1.019123),
	float3(-0.006166, -0.008043, 1.045482), float3(0.012998, 0.002299, 1.073049), float3(-0.013792, 0.008773, 1.101902),
	float3(0.005083, -0.018907, 1.132126), float3(0.010555, 0.020322, 1.163813), float3(-0.024718, -0.009027, 1.197063),
	float3(0.027573, -0.011382, 1.231983), float3(-0.014176, 0.030294, 1.268692), float3(-0.011125, -0.035470, 1.307319),
	float3(0.035487, 0.020565, 1.348003), float3(-0.043928, 0.009657, 1.390899), float3(0.028220, -0.040140, 1.436176),
	float3(0.006849, 0.052850, 1.484019), float3(-0.044088, -0.037158, 1.534633), float3(0.062123, -0.002569, 1.588244),
	float3(-0.047388, 0.047157, 1.645099), float3(0.003312, -0.071624, 1.705477), float3(0.049160, 0.058910, 1.769685),
	float3(-0.081214, -0.010927, 1.838064), float3(0.071718, -0.049899, 1.910997), float3(-0.020414, 0.090743, 1.988915),
	float3(-0.049164, -0.085798, 2.072299), float3(0.100042, 0.031916, 2.161696), float3(-0.101130, 0.046725, 2.257721),
	float3(0.045587, -0.108929, 2.361080), float3(0.042332, 0.117693, 2.472575), float3(-0.117201, -0.061598, 2.593131),
	float3(0.135465, -0.035708, 2.723815), float3(-0.080140, 0.124636, 2.865865), float3(-0.026540, -0.154427, 3.020733),
	float3(0.130985, 0.101442, 3.190127), float3(-0.174572, 0.014463, 3.376072), float3(0.125786, -0.135971, 3.580996),
	float3(-0.000956, 0.195913, 3.807830), float3(-0.139277, -0.153533, 4.060153), float3(0.218506, 0.020251, 4.342384),
	float3(-0.185167, 0.140535, 4.660052), float3(0.044111, -0.242474, 5.020170), float3(0.139309, 0.221377, 5.431781),
	float3(-0.268066, -0.073466, 5.906755), float3(0.263190, -0.135065, 6.461003), float3(-0.109652, 0.295769, 7.116369),
	float3(-0.127115, -0.312254, 7.903692), float3(0.326537, 0.154752, 8.867933), float3(-0.371441, 0.114519, 10.077632),
	float3(0.212370, -0.362371, 11.641928), float3(0.095862, 0.446412, 13.746795), float3(-0.407964, -0.289799, 16.736521),
	float3(0.550705, -0.068633, 21.327610), float3(-0.406411, 0.477468, 29.298947), float3(-0.026714, -0.732532, 46.613892),
	float3(0.657416, 0.667109, 113.316758)
};

// where a pixel of the scatter pass's target is, in world space (from its depth, in its view)
float3 ScatterPosition(ViewData view, float2 pixel, float depth)
{
	float2 ndc = (pixel + 0.5 - view.viewport.xy) / view.viewport.zw * 2.0 - 1.0;
	float4 position = mul(view.inverseViewProjection, float4(ndc, depth, 1.0));
	return position.xyz / position.w;
}

// how thick a scattering surface is at a pixel of the scatter pass, along the view: from its position to the nearest back
// face behind it (the Scatter Back pass), or -1 if none is
float ScatterThickness(float2 pixel, uint viewId, float3 position)
{
	float depth = gTextures[SHADER_TYPES_SCATTER_BACK_DEPTH_TEXTURE].Load(int3(int2(pixel), 0)).r;
	if (depth <= 0.0)
		return -1.0;
	ViewData view = gViewData[gPushConstants.frameIndex][viewId];
	return distance(position, ScatterPosition(view, floor(pixel), depth));
}

// the diffusion profile's scale (BurleyScale, per color) of a scattering material: the extinction (KHR_materials_volume's
// attenuation, at most 10 attenuation distances per color) gives the mean free path, the multiscatter color (the pixel's,
// textured) the albedo. none without a volume
float3 ScatterProfileScale(MaterialData material, float3 albedo)
{
	if (material.transmission.z <= 0.0)
		return 0.0;
	float3 extinction = max(-log(max(material.attenuationColor.rgb, 1e-4)), 0.1) / material.transmission.z;
	return BurleyScale(1.0 / extinction, albedo);
}

// the scattering light (ScatterLight) at a pixel of the scatter pass's target, blurred by its material's diffusion
// profile: from the samples of the same material around it, weighed by their distance in world space, the pattern turned
// by the pixel's place in a 4x4 tile (interleaved sampling: noise rather than copies of the image, which
// ComputeScatterDenoise's 4x4 filter cancels)
float3 SubsurfaceScattering(int2 center, ViewData view, float4 centerSample, float3 scale)
{
	Texture2D scatter = gTextures[SHADER_TYPES_SCATTER_TEXTURE];
	Texture2D scatterDepth = gTextures[SHADER_TYPES_SCATTER_DEPTH_TEXTURE];
	float maxScale = max(scale.r, max(scale.g, scale.b));
	if (maxScale <= 0.0)
		return centerSample.rgb;
	float centerDepth = scatterDepth.Load(int3(center, 0)).r;
	float3 centerPosition = ScatterPosition(view, float2(center), centerDepth);
	float metersPerPixel = distance(centerPosition, ScatterPosition(view, float2(center) + float2(1.0, 0.0), centerDepth));
	float maxRadiusPixels = maxScale / kScatterSampleScale / max(metersPerPixel, 1e-12);
	// within a pixel: the pixel is all of it
	if (maxRadiusPixels <= 1.0)
		return centerSample.rgb;

	float3 d = max(scale, kScatterMinRadius * maxScale);
	// each of a 4x4 tile's pixels turned by its own sixteenth of a turn, in an order that spreads neighbors apart
	static const uint kTileOrder[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
	float angle = 2.0 * kPi / 16.0 * float(kTileOrder[(uint(center.y) & 3u) * 4u + (uint(center.x) & 3u)]);
	float2x2 turn = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
	float2 low = view.viewport.xy;
	float2 high = view.viewport.xy + view.viewport.zw;
	float3 total = 0.0;
	float3 totalWeight = 0.0;
	for (uint sampleIt = 0; sampleIt < kScatterSampleCount; sampleIt++)
	{
		float3 profileSample = kScatterSamples[sampleIt];
		float2 at = floor(float2(center) + 0.5 + mul(turn, profileSample.xy) * maxRadiusPixels);
		if (any(at < low) || any(at >= high))
			continue;
		float4 neighbor = scatter.Load(int3(int2(at), 0));
		if (neighbor.a != centerSample.a)
			continue;
		float3 position = ScatterPosition(view, at, scatterDepth.Load(int3(int2(at), 0)).r);
		float3 weight = BurleyEval(d, distance(position, centerPosition)) * profileSample.z;
		total += weight * neighbor.rgb;
		totalWeight += weight;
	}
	return total / max(totalWeight, 1e-4);
}

// the Scatter Blur pass: the scattering light blurred, per pixel of the scatter pass's target (by the view it is in),
// with the noise of its tiled pattern, which ComputeScatterDenoise filters out
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeScatter(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D result = gRWTextures[SHADER_TYPES_SCATTER_NOISY_RW_TEXTURE];
	uint2 extent;
	result.GetDimensions(extent.x, extent.y);
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(dispatchThreadId.xy >= extent))
		return;
	float4 centerSample = gTextures[SHADER_TYPES_SCATTER_TEXTURE].Load(int3(pixel, 0));
	float3 light = centerSample.rgb;
	ViewData view;
	if (centerSample.a > 0.0 && ViewAt(pixel, view))
		light = SubsurfaceScattering(
			pixel, view, centerSample,
			ScatterProfileScale(gMaterialData[uint(centerSample.a) - 1u], gTextures[SHADER_TYPES_SCATTER_ALBEDO_TEXTURE].Load(int3(pixel, 0)).rgb));
	result[pixel] = float4(light, centerSample.a);
}

// the Scatter Denoise pass: the blurred light averaged over the 4x4 tile around each pixel, which holds every turn of the
// pattern once (as many samples as 16 pixels took), of the neighbors on the same surface: the same material, and in world
// space no farther than 8 times as far as they are on screen (for slopes) from the pixel. at edges fewer turns remain
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeScatterDenoise(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D result = gRWTextures[SHADER_TYPES_SCATTER_LIGHT_RW_TEXTURE];
	uint2 extent;
	result.GetDimensions(extent.x, extent.y);
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(dispatchThreadId.xy >= extent))
		return;
	Texture2D noisy = gTextures[SHADER_TYPES_SCATTER_NOISY_TEXTURE];
	Texture2D scatterDepth = gTextures[SHADER_TYPES_SCATTER_DEPTH_TEXTURE];
	float4 center = noisy.Load(int3(pixel, 0));
	ViewData view;
	if (center.a <= 0.0 || !ViewAt(pixel, view))
	{
		result[pixel] = center;
		return;
	}
	float centerDepth = scatterDepth.Load(int3(pixel, 0)).r;
	float3 centerPosition = ScatterPosition(view, float2(pixel), centerDepth);
	float metersPerPixel = distance(centerPosition, ScatterPosition(view, float2(pixel) + float2(1.0, 0.0), centerDepth));
	float2 low = view.viewport.xy;
	float2 high = view.viewport.xy + view.viewport.zw;
	float3 total = 0.0;
	float count = 0.0;
	for (int y = -1; y <= 2; y++)
		for (int x = -1; x <= 2; x++)
		{
			int2 at = pixel + int2(x, y);
			if (any(float2(at) < low) || any(float2(at) >= high))
				continue;
			float4 neighbor = noisy.Load(int3(at, 0));
			if (neighbor.a != center.a)
				continue;
			float3 position = ScatterPosition(view, float2(at), scatterDepth.Load(int3(at, 0)).r);
			if (distance(position, centerPosition) > 8.0 * metersPerPixel * length(float2(x, y)) + 1e-6)
				continue;
			total += neighbor.rgb;
			count += 1.0;
		}
	result[pixel] = float4(total / count, center.a);
}

// the blurred scattering light at a pixel (the Scatter Blur pass's), if the scatter pass drew this material there
bool ScatteredLight(float2 pixel, uint materialId, out float3 light)
{
	light = 0.0;
	if (gPushConstants.scatter == 0u)
		return false;
	float4 scattered = gTextures[SHADER_TYPES_SCATTER_LIGHT_TEXTURE].Load(int3(int2(pixel), 0));
	if (scattered.a != float(materialId + 1u))
		return false;
	light = scattered.rgb;
	return true;
}
