// KHR_materials_transmission and volume: the light from behind a surface, attenuated through it. a part of shaders.slang, which includes it after capi.h and sky.h

// what a volume lets through over a thickness (KHR_materials_volume): attenuationColor at attenuationDistance (0: none)
float3 VolumeAttenuation(float3 attenuationColor, float attenuationDistance, float thickness)
{
	return attenuationDistance > 0.0 ? pow(attenuationColor, thickness / attenuationDistance) : float3(1.0);
}

// the transmission texture holds the opaque scene compressed by each texel's brightest channel at the view's exposure,
// c / (1 + max(c) e), and its mips average that (Karis' reversible tonemap, as bloom's downsampling and resolves have it):
// a few texels far brighter than white weigh about as much as white, so a glint smaller than a pixel, which the jitter
// hits in some frames and not others, can't light up the blur of rough glass in front of it (the sun along the top
// shelf's front wire in CommercialRefrigerator, 5 to 20 times white at 2 of balanced's 18 jitter points, lit a blob over
// the bottles above it through the frosted door in those frames). TransmissionDecompress takes a lookup back,
// c / (1 - max(c) e), which an average of compressed texels keeps finite
float3 TransmissionCompress(float3 color, float exposure)
{
	return color / (1.0 + max(max(color.r, color.g), color.b) * exposure);
}

float3 TransmissionDecompress(float3 color, float exposure)
{
	return color / max(1.0 - max(max(color.r, color.g), color.b) * exposure, 1e-3);
}

// the Transmission pass: the opaque scene into the transmission texture's level 0, compressed (see TransmissionCompress)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeTransmissionCopy(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 pixel = int2(dispatchThreadId.xy);
	if (pixel.x >= int(gPushConstants.framebufferWidth) || pixel.y >= int(gPushConstants.framebufferHeight))
		return;
	float4 color = gTextures[SHADER_TYPES_RENDER_TARGET_TEXTURE].Load(int3(pixel, 0));
	uint viewId;
	ViewIndexAt(pixel, viewId);
	float exposure = gViewData[gPushConstants.frameIndex][viewId].exposure;
	gRWTextures[SHADER_TYPES_TRANSMISSION_RW_TEXTURE][pixel] = float4(TransmissionCompress(color.rgb, exposure), color.a);
}

// KHR_materials_transmission (and volume, dispersion): the light from behind a surface, along the ray refracted by ior:
// the opaque scene (SHADER_TYPES_TRANSMISSION_TEXTURE) where the ray leaves the volume (thickness along it; at the
// surface for thin walls), projected into the view, at a mip for the roughness (as the Khronos sample viewer: by the
// texture's size and the ior), and the environment where nothing opaque is there. attenuated through the volume.
float3 TransmittedLight(
	float3 normal, float3 view, float3 position, float roughness, float ior, float thickness, float3 attenuationColor,
	float attenuationDistance, uint viewId)
{
	float3 ray = refract(-view, normal, 1.0 / max(ior, 1e-4));
	if (dot(ray, ray) == 0.0) // total internal reflection
		ray = -view;
	ray = normalize(ray);
	float3 exit = position + ray * thickness;

	ViewData viewData = gViewData[gPushConstants.frameIndex][viewId];
	float4 clip = mul(viewData.viewProjection, float4(exit, 1.0));
	// where the ray leaves the volume, in the view. beyond the view's edges the scene wasn't drawn (clamping streaked the
	// edge's texels), so towards them, over its last 5%, the environment along the ray takes over: plausible rather than
	// right, but steady
	float2 local = clip.w > 0.0 ? clip.xy / clip.w * 0.5 + 0.5 : float2(-1.0);
	float2 edge = min(local, 1.0 - local);
	float inside = saturate(min(edge.x, edge.y) / 0.05);
	Texture2D scene = gTextures[SHADER_TYPES_TRANSMISSION_TEXTURE];
	uint width;
	uint height;
	uint levels;
	scene.GetDimensions(0, width, height, levels);
	float2 uv = (viewData.viewport.xy + saturate(local) * viewData.viewport.zw) / float2(width, height);
	float lod = log2(float(width)) * roughness * saturate(ior * 2.0 - 2.0);
	float4 behind = scene.SampleLevel(gSamplers[SHADER_TYPES_CLAMP_SAMPLER], uv, lod);
	// premultiplied: the clear color is transparent black
	float3 environment = EnvironmentRadiance(gEnvironment[0], ray, roughness);
	float3 seen = TransmissionDecompress(behind.rgb, viewData.exposure) + (1.0 - behind.a) * environment;
	float3 light = lerp(environment, seen, inside);
	// where the device has ray queries (View > Ray Traced Refractions): what the refracted ray meets, traced, behind the
	// surface or out of the view alike, sharp where the texture's mips wouldn't blur it much (see TracedShare), and where
	// the view doesn't show what is behind (toward its edges) whatever the roughness, in place of the environment: rough
	// glass, partly the texture's, faded to the sky toward the view's edges as untraced glass does
#if BVH_RAY_QUERY
	float blur = roughness * saturate(ior * 2.0 - 2.0);
	float traced = TracedRefractionsOn() ? TracedShare(blur) : 0.0;
	bool valid = false;
	float3 tracedLight = TracedRefractionsOn() && (traced > 0.0 || inside < 1.0) ? TracedRefraction(position, ray, valid) : 0.0;
	if (valid)
		light = lerp(lerp(tracedLight, seen, inside), tracedLight, traced);
#endif

	if (attenuationDistance > 0.0)
		light *= pow(max(attenuationColor, 1e-6), thickness / attenuationDistance);
	return light;
}
