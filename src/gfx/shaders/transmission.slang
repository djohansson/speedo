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

// the Transmission pass: the opaque scene into the transmission texture's level 0, compressed (see TransmissionCompress),
// through the air between the eye and it (the aerial perspective at its depth, which ComputeMain applies to what is drawn
// over it at the glass's depth alone: the glass takes the eye's stretch out again, see TransmittedLight)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeTransmissionCopy(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 pixel = int2(dispatchThreadId.xy);
	if (pixel.x >= int(gPushConstants.framebufferWidth) || pixel.y >= int(gPushConstants.framebufferHeight))
		return;
	uint frameIndex = gPushConstants.frameIndex;
	float4 color = gTextures[SHADER_TYPES_RENDER_TARGET_TEXTURE].Load(int3(pixel, 0));
	ViewData view;
	uint viewId;
	bool inView = ViewAt(pixel, view, viewId);
	AerialPerspectiveData aerial = gAerialPerspectiveData[frameIndex][0];
	if (aerial.enabled != 0u && inView && color.a > 0.0)
	{
		float3 light;
		float3 transmittance;
		AerialPerspectiveAtDepth(aerial, view, viewId, uint2(pixel), gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(pixel, 0)).r, light, transmittance);
		color.rgb = color.rgb * transmittance + color.a * light;
	}
	float exposure = inView ? view.exposure : gPushConstants.exposure;
	gRWTextures[SHADER_TYPES_TRANSMISSION_RW_TEXTURE][pixel] = float4(TransmissionCompress(color.rgb, exposure), color.a);
}

// the ray refracted into a surface (along normal, by ior) from view: along the view where it reflects all of it
float3 TransmittedRay(float3 normal, float3 view, float ior)
{
	float3 ray = refract(-view, normal, 1.0 / max(ior, 1e-4));
	if (dot(ray, ray) == 0.0) // total internal reflection
		ray = -view;
	return normalize(ray);
}

// how much of what is behind a surface the view shows where its ray leaves the volume (exit): beyond the view's edges the
// scene wasn't drawn (clamping streaked the edge's texels), so towards them, over its last 5%, the environment along the
// ray takes over: plausible rather than right, but steady. local: where in the view's viewport
float TransmittedInView(float3 exit, ViewData viewData, out float2 local)
{
	float4 clip = mul(viewData.viewProjection, float4(exit, 1.0));
	local = clip.w > 0.0 ? clip.xy / clip.w * 0.5 + 0.5 : float2(-1.0);
	float2 edge = min(local, 1.0 - local);
	return saturate(min(edge.x, edge.y) / 0.05);
}

// how much of the light from behind a surface its traced refraction gives (see TransmittedLight): sharp where the
// transmission texture's mips wouldn't blur it much (the blur its lookup takes, by the roughness and the ior)
float TransmittedTracedShare(float roughness, float ior)
{
	return TracedShare(roughness * saturate(ior * 2.0 - 2.0));
}

// whether a fragment traces its refraction (View > Ray Traced Refractions, where the device has ray queries; see
// TransmittedLight), and along which direction: the refracted ray into a volume (thickness), the view's through a thin
// wall (the screen space lookup's place is the surface's own, behind it)
bool TransmittedTraced(
	float3 normal, float3 view, float3 position, float roughness, float ior, float thickness, uint viewId, out float3 direction)
{
	float3 ray = TransmittedRay(normal, view, ior);
	direction = thickness > 0.0 ? ray : -view;
	float2 local;
	return TracedRefractionsOn() && !TracedGlossyHere() &&
		   (TransmittedTracedShare(roughness, ior) > 0.0 ||
			TransmittedInView(position + ray * thickness, gViewData[gPushConstants.frameIndex][viewId], local) < 1.0);
}

// KHR_materials_transmission (and volume, dispersion): the light from behind a surface, along the ray refracted by ior:
// the opaque scene (SHADER_TYPES_TRANSMISSION_TEXTURE) where the ray leaves the volume (thickness along it; at the
// surface for thin walls), projected into the view, at a mip for the roughness (as the Khronos sample viewer: by the
// texture's size and the ior), and the environment where nothing opaque is there. attenuated through the volume.
// traced: what the refracted ray met, traced (TracedFragmentRays, where TransmittedTraced says: rgb, and in a 1 where it is
// known), behind the surface or out of the view alike, by TransmittedTracedShare, and where the view doesn't show what is
// behind (toward its edges) whatever the roughness, in place of the environment: rough glass, partly the texture's, faded
// to the sky toward the view's edges as untraced glass does
float3 TransmittedLight(
	float3 normal, float3 view, float3 position, float roughness, float ior, float thickness, float3 attenuationColor,
	float attenuationDistance, uint viewId, float4 traced)
{
	float3 ray = TransmittedRay(normal, view, ior);
	float3 exit = position + ray * thickness;

	ViewData viewData = gViewData[gPushConstants.frameIndex][viewId];
	float2 local;
	float inside = TransmittedInView(exit, viewData, local);
	Texture2D scene = gTextures[SHADER_TYPES_TRANSMISSION_TEXTURE];
	uint width;
	uint height;
	uint levels;
	scene.GetDimensions(0, width, height, levels);
	float2 uv = (viewData.viewport.xy + saturate(local) * viewData.viewport.zw) / float2(width, height);
	float lod = log2(float(width)) * roughness * saturate(ior * 2.0 - 2.0);
	float4 behind = scene.SampleLevel(gSamplers[SHADER_TYPES_CLAMP_SAMPLER], uv, lod);
	// premultiplied: the clear color is transparent black. the air: the copy has it from the eye to what it shows (see
	// ComputeTransmissionCopy), and ComputeMain applies the eye's stretch to this surface once this fragment is drawn, so
	// that stretch is taken out here, by this surface's distance at the copy's place, leaving the air between the surface
	// and what it shows; the environment, where nothing was drawn, is seen through the fog near the ground toward the sky
	// along the ray (FogToSky: the sky has the rest of the air already)
	AerialPerspectiveData aerial = gAerialPerspectiveData[gPushConstants.frameIndex][0];
	float3 seen = TransmissionDecompress(behind.rgb, viewData.exposure);
	if (aerial.enabled != 0u && behind.a > 0.0)
	{
		float3 toHereLight;
		float3 toHereTransmittance;
		AerialPerspectiveAt(aerial, saturate(local), viewId, distance(viewData.eyePosition.xyz, position), toHereLight, toHereTransmittance);
		seen = max(seen - behind.a * toHereLight, 0.0) / max(toHereTransmittance, 1e-6);
	}
	float3 environment = EnvironmentRadiance(gEnvironment[0], ray, roughness);
	{
		float3 fogLight;
		float3 fogTransmittance;
		FogToSky(aerial, ray, position.y, fogLight, fogTransmittance);
		environment = environment * fogTransmittance + fogLight;
	}
	seen += (1.0 - behind.a) * environment;
	float3 light = lerp(environment, seen, inside);
	if (traced.a > 0.0)
		light = lerp(lerp(traced.rgb, seen, inside), traced.rgb, TransmittedTracedShare(roughness, ior));

	if (attenuationDistance > 0.0)
		light *= pow(max(attenuationColor, 1e-6), thickness / attenuationDistance);
	return light;
}
