// KHR_materials_transmission and volume: the light from behind a surface, attenuated through it. a part of shaders.slang, which includes it after capi.h and sky.h

// what a volume lets through over a thickness (KHR_materials_volume): attenuationColor at attenuationDistance (0: none)
float3 VolumeAttenuation(float3 attenuationColor, float attenuationDistance, float thickness)
{
	return attenuationDistance > 0.0 ? pow(attenuationColor, thickness / attenuationDistance) : float3(1.0);
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
	float3 light = lerp(environment, behind.rgb + (1.0 - behind.a) * environment, inside);

	if (attenuationDistance > 0.0)
		light *= pow(max(attenuationColor, 1e-6), thickness / attenuationDistance);
	return light;
}
