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

// whether a transmissive surface of a thickness (KHR_materials_volume's, 0 without) is a solid, refracting into it and out
// of it, rather than an infinitely thin wall, which bends nothing (glTF's reading of transmission without a volume):
// a volume's, and every one where TRACED_SOLID_TRANSMISSION says (Scene::Settings::solidTransmission: the glTF research
// set's dielectrics, pbrt's solids, which its references refract as solids). a solid without a thickness absorbs
// nothing, and the screen space lookup reads at the surface itself (where its ray leaves isn't known)
bool TransmissionSolid(float thickness)
{
	return thickness > 0.0 || (gPushConstants.traced & TRACED_SOLID_TRANSMISSION) != 0u;
}

// how much of the light from behind a surface its traced refraction gives (see TransmittedLight): all of it where the
// transmission texture's mips wouldn't blur it much (the blur its lookup takes, by the roughness and the ior), none from
// kTracedRoughnessEnd
float TransmittedTracedShare(float roughness, float ior)
{
	return TracedShare(roughness * saturate(ior * 2.0 - 2.0));
}

// the traced refraction's ray (sample: dispersion's color): a ray of the surface's lobe, the view refracted by ior about
// a microfacet normal of the GGX lobe of the roughness, by the normals visible from view (Walter et al. 2007's
// transmission, sampled as Heitz 2018's TracedVisibleNormal), another a frame and pixel (averaged over frames by the
// temporal upscaler), then (a thin wall) out through its other face, as parallel to this one, so that a smooth wall's
// goes on as the view; the sharp refraction where the normal refracts nothing or out of the surface. the sharp ray alone
// drew what is behind rough glass sharp in the traced share (a quarter of the light at 0.32, lamp's frosted bulb and
// bathroom2's jar: the bulb's emitter's edge in grainy notches along its rim, where the reference's bulb is a glow;
// docs/research-scenes-plan.md, C2). three rays a pixel (in the slots of dispersion's colors) were a little smoother and
// cost Main Transmissive 0.45 ms a frame more in lamp at 1440p, 0.25 in bathroom2, 0.14 in Bistro at night; the traced
// share as far as the glossy reflections' (all to 0.45, none from 0.6) drew bathroom2's jar's edges orange, the counter
// below seen through the bottle's sides, where the reference's show the wall behind as the mips do
static const float kTransmittedLobeShare = 0.97;

float3 TransmittedLobeRay(float3 normal, float3 view, float roughness, float ior, bool solid, uint sample)
{
	float3 sharp = TransmittedRay(normal, view, ior);
	float alpha = max(roughness * roughness, 1e-3);
	float3 tangent = normalize(abs(normal.z) < 0.999 ? cross(float3(0.0, 0.0, 1.0), normal) : cross(float3(1.0, 0.0, 0.0), normal));
	float3 bitangent = cross(normal, tangent);
	float3 local = normalize(float3(dot(view, tangent), dot(view, bitangent), max(dot(view, normal), 1e-4)));
	// (the lobe without its outermost normals, kTransmittedLobeShare of it: see GlassLight)
	float2 u = TracedRandom(gFragmentPixel, TracedFrame() * 3u + sample);
	u.x *= kTransmittedLobeShare;
	float3 h = TracedVisibleNormal(local, alpha, u);
	float3 microfacet = normalize(h.x * tangent + h.y * bitangent + h.z * normal);
	float3 into = refract(-view, microfacet, 1.0 / max(ior, 1e-4));
	if (dot(into, into) == 0.0 || dot(into, normal) >= 0.0)
		into = sharp;
	into = normalize(into);
	if (solid)
		return into;
	float3 through = refract(into, normal, ior);
	return dot(through, through) == 0.0 ? -view : normalize(through);
}

// the rough glass's light: a rough transmissive surface's traced refraction is one ray of its lobe a pixel a frame
// (TransmittedLobeRay), which the temporal upscaler was left to average: enough for a frosted bulb around its emitter,
// most of whose rays meet it, and not where few of a pixel's rays meet something far brighter than the rest: the glTF
// research set's spaceship's display, a bar of emission 20 behind its dome's glass (roughness 0.1), drew red single
// pixel specks around itself for good, the lobe's rare rays that meet it, each a frame's (the user's report,
// 2026-11-23). while nothing but the view may have changed what the glass shows and the view is still
// (TRACED_GLASS_STILL, EmitterViewStill) the fragment's rays' light is the mean of the frames', at most
// kGlassFrameCount of them: the last frame's mean at its pixel (SHADER_TYPES_GLASS_HISTORY_TEXTURE, its alpha the
// frames it is of) with this frame's, written beside the color (gFragmentGlass, the transmissive phase's second
// attachment, which the depth test keeps the nearest fragment's of) for the Glass History pass to keep. the rays'
// light alone, not the fragment's color: what the glass reflects and its edges are the upscaler's still, which
// resolves them finer than a mean at the pixel would. from kGlassRoughnessNone to kGlassRoughnessFull: a smooth
// surface's ray is the same every frame, and what it shows is the upscaler's to resolve too. where the view moves
// or anything changes, a frame's rays as before
static const float kGlassFrameCount = 128.0;
static const float kGlassRoughnessNone = 0.02;
static const float kGlassRoughnessFull = 0.06;
static const int kGlassReach = 2;
// what the fragment being shaded keeps of its rays' light for the next frame: the mean, and in alpha the frames it is
// of (0: none), see GlassLight
static float4 gFragmentGlass = float4(0.0, 0.0, 0.0, 0.0);

// traced: TracedFragmentRays' light, whose refraction rays' (one, or one a color with dispersion, each its own color's)
// are replaced by their mean over the frames
void GlassLight(float roughness, bool dispersion, inout float4 traced[kTracedFragmentRayCount])
{
	float share = smoothstep(kGlassRoughnessNone, kGlassRoughnessFull, roughness);
	float4 red = traced[kTracedFragmentRefraction];
	float4 green = traced[kTracedFragmentRefraction + (dispersion ? 1u : 0u)];
	float4 blue = traced[kTracedFragmentRefraction + (dispersion ? 2u : 0u)];
	if (share <= 0.0 || min(red.a, min(green.a, blue.a)) <= 0.0)
		return;
	float3 light = float3(red.r, green.g, blue.b);
	float count = 1.0;
	ViewData view = gViewData[gPushConstants.frameIndex][gShadingViewId];
	if ((gPushConstants.traced & TRACED_GLASS_STILL) != 0u && view.previousEyePosition.w > 1.5)
	{
		float4 kept = gTextures[SHADER_TYPES_GLASS_HISTORY_TEXTURE].Load(int3(int2(gFragmentPixel), 0));
		if (kept.a >= 1.0)
		{
			count = min(kept.a, kGlassFrameCount - 1.0) + 1.0;
			light = lerp(kept.rgb, light, 1.0 / count);
		}
	}
	gFragmentGlass = float4(light, count);
	// and what is shown is that mean with the means the pixels around it kept (kGlassReach each way, by a tent and by
	// the frames each is of), which the fragment doesn't keep: a pixel's own mean of kGlassFrameCount rays is smooth
	// where a tenth of its rays meet what is bright, and further out, where one in a hundred does, the spaceship's
	// display had a lattice of dim red dots around its glow (the rays' sequence, a frame's place in it by the pixel:
	// TracedRandom), 20 / 128 against a ground of 0.02. kept, the neighbours' share would spread a pixel further every
	// frame
	if (count > 1.0)
	{
		float3 sum = light * count * float((kGlassReach + 1) * (kGlassReach + 1));
		float weights = count * float((kGlassReach + 1) * (kGlassReach + 1));
		for (int y = -kGlassReach; y <= kGlassReach; y++)
			for (int x = -kGlassReach; x <= kGlassReach; x++)
			{
				if (x == 0 && y == 0)
					continue;
				float4 beside = gTextures[SHADER_TYPES_GLASS_HISTORY_TEXTURE].Load(int3(int2(gFragmentPixel) + int2(x, y), 0));
				float weight = beside.a * float((kGlassReach + 1 - abs(x)) * (kGlassReach + 1 - abs(y)));
				sum += weight * beside.rgb;
				weights += weight;
			}
		light = sum / weights;
	}
	float3 shown = lerp(float3(red.r, green.g, blue.b), light, share);
	if (dispersion)
	{
		traced[kTracedFragmentRefraction].r = shown.r;
		traced[kTracedFragmentRefraction + 1u].g = shown.g;
		traced[kTracedFragmentRefraction + 2u].b = shown.b;
	}
	else
		traced[kTracedFragmentRefraction].rgb = shown;
}

// the Glass History pass: the rough glass's light the transmissive phase's fragments wrote this frame, kept for the
// next frame's (see GlassLight)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeGlassHistory(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 pixel = int2(dispatchThreadId.xy);
	if (pixel.x >= int(gPushConstants.framebufferWidth) || pixel.y >= int(gPushConstants.framebufferHeight))
		return;
	gRWTextures[SHADER_TYPES_GLASS_HISTORY_RW_TEXTURE][pixel] = gTextures[SHADER_TYPES_GLASS_LIGHT_TEXTURE].Load(int3(pixel, 0));
}

// whether a fragment traces its refraction (View > Rendering Features > Ray Traced Refractions, where the device has ray queries; see
// TransmittedLight), and along which direction: a ray of its lobe (TransmittedLobeRay; sample: dispersion's color), into a
// volume (thickness), or through a thin wall (the screen space lookup's place is the surface's own, behind it)
bool TransmittedTraced(
	float3 normal, float3 view, float3 position, float roughness, float ior, float thickness, uint viewId, uint sample, out float3 direction)
{
	float3 ray = TransmittedRay(normal, view, ior);
	direction = TransmittedLobeRay(normal, view, roughness, ior, TransmissionSolid(thickness), sample);
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
	// attenuated through the volume: the untraced by its thickness (KHR_materials_volume's stand in for the path's length,
	// as a rasterizer takes it); the traced along its path inside (see GlassPathStep's alongPath), as a ray tracer takes it
	float3 absorbed = attenuationDistance > 0.0 ? pow(max(attenuationColor, 1e-6), thickness / attenuationDistance) : float3(1.0);
	float3 light = lerp(environment, seen, inside) * absorbed;
	if (traced.a > 0.0)
		light = lerp(lerp(traced.rgb, seen * absorbed, inside), traced.rgb, TransmittedTracedShare(roughness, ior));
	return light;
}
