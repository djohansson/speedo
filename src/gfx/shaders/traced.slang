// the traced reflections and refractions (PushConstants::traced, TRACED_*; View > Ray Traced Reflections and
// Refractions): where the device has ray queries (BVH_RAY_QUERY), the radiance along a smooth surface's reflection
// (TracedReflection, see Shade) and the light through a transmissive one (TracedRefraction, see TransmittedLight), traced
// through the model's acceleration structures (gBvhScene[frame], BvhTraceScene) and shaded where the ray meets something
// (TracedHitRadiance), the environment where it meets nothing. a fragment's (Main's blended surfaces, the transmissive
// ones, the forward path) sharp: a ray along the reflection's dominant direction (or the refracted ray), all of the
// reflection below kTracedRoughnessFull, faded into the glossy one the probes and the environment give (or the
// transmission texture's mips) by kTracedRoughnessEnd. the visibility buffer's glossy: a ray a pixel of the GGX lobe
// (TracedGlossyReflection), up to kTracedGlossyRoughnessEnd, filtered over the surface and over frames by the Reflection
// passes (ComputeReflectionX, Y and Temporal), which add it to the main color. a part of shaders.slang, which includes it
// after capi.h and sky.h

// the roughness up to which a reflection (or a transmission's blur, see TransmittedLight) is all traced, and from which
// none of it is: a ray along the dominant direction is a mirror's reflection, too sharp for a glossy surface
static const float kTracedRoughnessFull = 0.15;
static const float kTracedRoughnessEnd = 0.4;
// how far the rays go, in the scene's units (any scene's: what they meet is nearer)
static const float kTracedFar = 1.0e30;
// how many transmissive surfaces a reflection's ray goes through (see TracedRadiance), the one it met first among them:
// past them the caller keeps what it would have taken untraced
static const uint kTracedReflectionGlassSurfaceCount = 2u;
// the share of a reflection's light the specular takes (the split sum's terms by the occlusion, as luminance) below which
// none of it is traced, and from which all of it: below, what a trace changes wouldn't show. a dielectric's is about 0.04
// at normal incidence (glass, water: traced), and grows toward grazing angles; an obj material of a faint Ks (its Ns
// smooth, San Miguel's walls, floors and trees) a few thousandths, which traced cost San Miguel 1.85 ms a frame
static const float kTracedStrengthNone = 0.005;
static const float kTracedStrengthFull = 0.015;

// how much of a reflection (or transmission) of a roughness is traced
float TracedShare(float roughness)
{
	return 1.0 - smoothstep(kTracedRoughnessFull, kTracedRoughnessEnd, roughness);
}

// and of a reflection of a roughness whose light the specular takes a share of (strength, see kTracedStrengthNone)
float TracedShare(float roughness, float strength)
{
	return TracedShare(roughness) * smoothstep(kTracedStrengthNone, kTracedStrengthFull, strength);
}

// the roughness up to which the visibility buffer's reflections are all traced (glossy: a ray a pixel sampled from the
// GGX lobe, filtered over the surface and over frames, see TracedGlossyReflection), and from which none of them: rougher,
// the probes' glossy reflection is as good, and the rays' noise more than the filter takes out
static const float kTracedGlossyRoughnessFull = 0.45;
static const float kTracedGlossyRoughnessEnd = 0.6;
// where a glossy ray meets nothing, the hit distance its filter takes (the sky's blur as wide as the filter reaches; a
// half float holds it)
static const float kTracedMissDistance = 60000.0;
// how many frames the glossy reflections' history holds, from a mirror's (what it reflects moves across the surface as
// the eye does, which reprojecting the surface doesn't follow: a longer history trails) to the roughest traced (whose
// rays' noise needs them)
static const float kReflectionFramesSharp = 2.0;
static const float kReflectionFramesRough = 24.0;
// and where what the surface reflects didn't slide across it (a still view, or one turned about the eye) and nothing
// else may change what the reflections see (TRACED_SETTLED, and no ray around met what moves), unclamped: the frame's
// filtered 3x3 is smooth, so its box held the history to the frame's own noise, and ChronographWatch's strap, its rays
// meeting the sky in some weave cells and the case in others, changed by ten times as much as untraced every frame
static const float kReflectionFramesStill = 64.0;
static const float4 kReflectionUnknown = float4(0.0, 0.0, 0.0, -1.0);

// a glossy sample's alpha (and its filtered ones'): its hit distance, or -2 less it where what it met moves, whose
// reflection a still view's history mustn't hold (see ComputeReflectionTemporal); kReflectionUnknown's -1 where none is
// traced
float ReflectionPackDistance(float distance, bool moving)
{
	return moving ? -2.0 - distance : distance;
}

bool ReflectionUnknown(float packed)
{
	return packed > -1.5 && packed < 0.0;
}

bool ReflectionMoving(float packed)
{
	return packed <= -1.5;
}

float ReflectionDistance(float packed)
{
	return ReflectionMoving(packed) ? -2.0 - packed : packed;
}

// a glossy pixel's weight's alpha: its roughness, 2 more where its normal map turns its normal from the geometry's by more
// than about 6 degrees (its reflection is its own direction's, which the image of a mirror doesn't follow: see
// ComputeReflectionTemporal, which takes any in the 3x3 around); -1 where none is traced
float ReflectionPackRoughness(float roughness, bool bumpy)
{
	return bumpy ? roughness + 2.0 : roughness;
}

float ReflectionRoughness(float packed)
{
	return packed >= 1.5 ? packed - 2.0 : packed;
}

bool ReflectionBumpy(float packed)
{
	return packed >= 1.5;
}
// the brightest a glossy ray's sample is taken as, in multiples of white at the view's exposure (see
// TracedGlossyReflection)
static const float kReflectionMostWhite = 8.0;
// what the glossy reflections take a sample as a share of (see Shade) is what the probes and the environment predict
// along the ray and this much of white at the view's exposure: below it the prediction is too dark for its detail to show,
// and the share is the sample itself (at night, a lit window met by rays where the sky predicts nothing)
static const float kReflectionPredictionLeast = 0.02;

float TracedGlossyShare(float roughness, float strength)
{
	return (1.0 - smoothstep(kTracedGlossyRoughnessFull, kTracedGlossyRoughnessEnd, roughness)) *
		   smoothstep(kTracedStrengthNone, kTracedStrengthFull, strength);
}

bool TracedReflectionsOn()
{
	return (gPushConstants.traced & TRACED_REFLECTIONS) != 0u;
}

bool TracedRefractionsOn()
{
	return (gPushConstants.traced & TRACED_REFRACTIONS) != 0u;
}

// the frames the glossy reflections have run (their rays' sampling, their history's parity)
uint TracedFrame()
{
	return gPushConstants.traced >> TRACED_FRAME_SHIFT;
}

// whether the shading here is the visibility buffer's (compute), whose reflections are glossy and left to the Reflection
// passes, rather than a fragment's (sharp, traced in place: see TracedReflection)
bool TracedGlossyHere()
{
	__stage_switch
	{
	case compute:
		return true;
	default:
		return false;
	}
}

// the visibility buffer's shading's glossy reflection at a pixel: the ray's radiance as a share of what was predicted
// along it and its hit distance, and the specular's weight of the pixel's own prediction with the surface's roughness
// (see Shade; ComputeReflectionTemporal adds their product to the main color); kReflectionUnknown in both where none is
// traced. in compute only: a fragment shader's storage writes turn its early depth test off, even on a path never taken
// (see GiMarch's cap)
void TracedReflectionStore(uint2 pixel, float4 sample, float4 weight)
{
	__stage_switch
	{
	case compute:
		gRWTextures[SHADER_TYPES_REFLECTION_RAW_RW_TEXTURE][pixel] = sample;
		gRWTextures[SHADER_TYPES_REFLECTION_WEIGHT_RW_TEXTURE][pixel] = weight;
		return;
	case fragment:
		return;
	}
}

#if BVH_RAY_QUERY
// a ray cone (Akenine-Moller et al., "Texture Level of Detail Strategies for Real-Time Ray Tracing"): its width where it
// starts and how fast it widens along the ray (radians), which picks the mip level its hit samples textures at: without,
// a reflected texture's every texel shimmers as the camera moves
struct TracedCone
{
	float width;
	float spread;
};

// the cone of the pixel the shading's view sees position at: as wide as the pixel there, widening as the pixel's angle
// does (a mirror's reflection keeps it; a curved surface's would widen it more, which is left out)
TracedCone TracedPixelCone(float3 position)
{
	ViewData view = gViewData[gPushConstants.frameIndex][gShadingViewId];
	float4 clip = mul(view.viewProjection, float4(position, 1.0));
	float3 ndc = clip.xyz / clip.w;
	float4 beside = mul(view.inverseViewProjection, float4(ndc.x, ndc.y + 2.0 / max(view.viewport.w, 1.0), ndc.z, 1.0));
	TracedCone cone;
	cone.width = max(distance(beside.xyz / beside.w, position), 1e-6);
	cone.spread = cone.width / max(distance(view.eyePosition.xyz, position), 1e-6);
	return cone;
}

// the mip level a cone as wide as width (in world space) samples a texture view at, on a triangle of a world area whose
// corners' texcoords are texCoords: the texels across the width, by the triangle's texels per world area, plus the view's
// lod bias (as the main pass's, see SampleView)
float TracedConeLevel(TextureView view, float4 texCoords[3], float worldArea, float width)
{
	float2 uv0 = ViewTexCoord(view, texCoords[0]);
	float2 uv1 = ViewTexCoord(view, texCoords[1]);
	float2 uv2 = ViewTexCoord(view, texCoords[2]);
	float uvArea = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y)) * 0.5;
	uint textureWidth;
	uint textureHeight;
	uint levels;
	gTextures[NonUniformResourceIndex(view.textureId)].GetDimensions(0, textureWidth, textureHeight, levels);
	float texelsPerMeter = sqrt(uvArea * float(textureWidth) * float(textureHeight) / max(worldArea, 1e-12));
	return log2(max(width * texelsPerMeter, 1.0)) + gViewData[gPushConstants.frameIndex][gShadingViewId].textureLodBias;
}

// a texture view sampled at a hit's texcoords, at the level a cone of width samples it at
float4 TracedSample(uint viewId, float4 texCoords[3], float3 weights, float worldArea, float width)
{
	TextureView view = gTextureViews[viewId];
	float4 texCoord01 = texCoords[0] * weights.x + texCoords[1] * weights.y + texCoords[2] * weights.z;
	return SampleViewLevel(view, ViewTexCoord(view, texCoord01), TracedConeLevel(view, texCoords, worldArea, width));
}

// the radiance off what a ray (along direction) met, toward where it came from: the hit's material at its texcoords (its
// base color, metallic and roughness, the specular-glossiness model's too; not the extensions' layers, nor its normal
// map), lit as the voxels are (the lights of its cell of the finest clip holding it, see GiDirectRadiance, each with a
// shadow ray; every light where the indirect lighting is off) and by the indirect lighting at it (the probes' diffuse,
// and the environment's specular as much as they see of it, as Shade's), plus its emission. not the emissive surfaces'
// light where they are lights (see EmitterIrradiance, a screen's worth of traced pixels). a double sided triangle's back
// face as its front (see TracedRadiance for a single sided one's). coneWidth: the ray's cone's width at the hit
float3 TracedHitRadiance(float3 direction, BvhHit hit, float3 position, float coneWidth)
{
	uint frameIndex = gPushConstants.frameIndex;
	MaterialData material = gMaterialData[hit.flags >> BVH_TRIANGLE_MATERIAL_SHIFT];
	VertexP3fN3fTa4fT014fC4f corners[3] = {
		gVertexBuffer[gIndexBuffer[hit.firstIndex]],
		gVertexBuffer[gIndexBuffer[hit.firstIndex + 1u]],
		gVertexBuffer[gIndexBuffer[hit.firstIndex + 2u]],
	};
	float3 weights = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics.x, hit.barycentrics.y);
	float4 texCoords[3] = { corners[0].texCoord01, corners[1].texCoord01, corners[2].texCoord01 };
	// the triangle's area in world space (what moves: in the mesh's, at rest), and its vertices' normal placed by its
	// instance, facing the side the ray came from (what moves: the triangle's own, as the frame placed it)
	float3 positions[3] = { corners[0].position, corners[1].position, corners[2].position };
	float3 normal = hit.normal;
	if (hit.modelInstance != ~0u)
	{
		ModelInstance instance = gModelInstances[frameIndex][hit.modelInstance];
		for (uint cornerIt = 0; cornerIt < 3u; cornerIt++)
			positions[cornerIt] = mul(instance.modelTransform, float4(positions[cornerIt], 1.0)).xyz;
		float3 interpolated = corners[0].normal * weights.x + corners[1].normal * weights.y + corners[2].normal * weights.z;
		if (dot(interpolated, interpolated) > 1e-8)
		{
			float3 placed = normalize(mul(instance.inverseTransposeModelTransform, float4(interpolated, 0.0)).xyz);
			normal = dot(placed, hit.normal) < 0.0 ? -placed : placed;
		}
	}
	float worldArea = length(cross(positions[1] - positions[0], positions[2] - positions[0])) * 0.5;

	// the material, as ShadeFragment takes it (the vertex colors hold the diffuse color)
	float4 color = (corners[0].color * weights.x + corners[1].color * weights.y + corners[2].color * weights.z) * material.color;
	if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
		color *= TracedSample(material.baseColorView, texCoords, weights, worldArea, coneWidth);
	float3 emission = material.emissive.rgb;
	if (any(emission > 0.0) && (material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
		emission *= TracedSample(material.emissiveView, texCoords, weights, worldArea, coneWidth).rgb;
	if ((material.flags & MATERIAL_FLAG_UNLIT) != 0u)
		return color.rgb;
	Surface surface;
	surface.occlusion = 1.0;
	surface.normal = normal;
	surface.geometryNormal = hit.normal;
	surface.clearcoat = 0.0;
	surface.clearcoatRoughness = 0.0;
	surface.clearcoatNormal = normal;
	surface.sheenColor = 0.0;
	surface.sheenRoughness = 0.0;
	surface.transmitted = 0.0;
	surface.anisotropy = 0.0;
	surface.anisotropyT = 0.0;
	surface.anisotropyB = 0.0;
	surface.iridescence = 0.0;
	surface.iridescenceFresnel = 0.0;
	surface.diffuseTransmission = 0.0;
	surface.diffuseTransmissionColor = 0.0;
	surface.retroreflection = 0.0;
	float4 specularColor = float4(material.specularColor.rgb, 1.0);
	if ((material.flags & MATERIAL_FLAG_SPECULAR_GLOSSINESS) != 0u)
	{
		surface.f0 = specularColor.rgb;
		surface.f90 = 1.0;
		surface.diffuseColor = color.rgb * (1.0 - max(surface.f0.r, max(surface.f0.g, surface.f0.b)));
		surface.roughness = 1.0 - saturate(material.roughness);
	}
	else
	{
		float metallic = material.metallic;
		float roughness = material.roughness;
		if ((material.flags & MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE) != 0u)
		{
			float2 sample = TracedSample(material.metallicRoughnessView, texCoords, weights, worldArea, coneWidth).rg;
			roughness *= sample.r;
			metallic *= sample.g;
		}
		float ior = material.specularColor.a;
		float reflectance = (ior - 1.0) / (ior + 1.0);
		float3 dielectricF0 = min(reflectance * reflectance * specularColor.rgb, 1.0) * material.specular;
		surface.f0 = lerp(dielectricF0, color.rgb, metallic);
		surface.f90 = lerp(material.specular, 1.0, metallic);
		surface.diffuseColor = lerp(color.rgb, float3(0.0), metallic);
		surface.roughness = roughness;
	}

	float3 view = -direction;
	float nDotV = max(dot(normal, view), 1e-4);
	float3 radiance = emission;

	// the lights: of the hit's cell of the finest clip whose box around the eye holds it (as the voxels are lit), else
	// every one
	GiData gi = GiClip(0);
	bool cells = gi.enabled != 0u;
	uint2 range = uint2(0u, gPushConstants.lightCount);
	if (cells)
	{
		uint clipIt = 0;
		for (; clipIt + 1u < gi.clipCount; clipIt++)
		{
			GiClipView clipView = GiClipViewAt(clipIt);
			if (all(abs(position - clipView.fadeCenter.xyz) < clipView.fadeExtent.xyz))
				break;
		}
		gi = GiClip(clipIt);
		float3 size = float3(GiLevelSize(gi, 0));
		float3 at = clamp((position - gi.volumeOrigin.xyz) / gi.volumeOrigin.w, 0.0, size - 1e-3);
		uint3 cell = min(uint3(at / size * float(SHADER_TYPES_GI_LIGHT_GRID)), uint3(SHADER_TYPES_GI_LIGHT_GRID - 1));
		range = gGiLightCells[frameIndex][gi.lightCellFirst + (cell.z * SHADER_TYPES_GI_LIGHT_GRID + cell.y) * SHADER_TYPES_GI_LIGHT_GRID + cell.x];
	}
	// the shadow rays start off the surface toward the light, by the cone's width there and a ten thousandth of the
	// distance (the hit's rounding). they meet no glass (its structure is the caustics' and the refractions' own), so
	// what the transmissive surfaces let through to the hit, and the caustics' light, are the shadow transmittance's where
	// the light's views hold the hit (see ShadowTransmittance), as the main pass takes them: without, the ground seen
	// through CausticSpheres' clear sphere was lit as if the sphere weren't there, neither its shadow nor its caustic
	bool transmittance = (gPushConstants.shadows & SHADER_TYPES_SHADOWS_TRANSMITTANCE) != 0u;
	float push = max(1.5 * coneWidth, 1e-4 * hit.t);
	for (uint lightIt = 0; lightIt < range.y; lightIt++)
	{
		uint lightIndex = cells ? gGiLightIndices[frameIndex][range.x + lightIt] : lightIt;
		LightData light = gLights[lightIndex];
		float3 toLight;
		float falloff = LightFalloff(light, position, toLight);
		float nDotL = dot(normal, toLight);
		if (falloff <= 0.0 || nDotL <= 0.0)
			continue;
		float side = dot(hit.normal, toLight) >= 0.0 ? 1.0 : -1.0;
		float reach = light.type == LIGHT_TYPE_DIRECTIONAL ? kTracedFar : distance(light.positionRange.xyz, position) - push;
		if (BvhHitSomething(BvhTraceScene(position + hit.normal * side * push, toLight, reach, true, true, GiAlphaMask())))
			continue;
		float3 through = 1.0;
		ShadowMapPlace place;
		if (transmittance && PlaceInShadowMap(lightIndex, light, position, hit.normal, toLight, place) == kShadowMapPlaced)
			through = ShadowTransmittance(place, true);
		radiance += BaseLobe(surface, view, toLight, nDotL) * nDotL * light.intensity.rgb * falloff * through;
	}

	float3 indirect;
	float3 specularRadiance;
	TracedIndirect(position, normal, direction, surface.roughness, indirect, specularRadiance);
	radiance += surface.diffuseColor * indirect +
				EnvironmentBrdfApprox(surface.f0, surface.f90, surface.roughness, nDotV) * specularRadiance;
	return radiance;
}

// the indirect lighting at a hit at position (normal, met along direction, of a roughness), as Shade takes it: the probes'
// diffuse where they cover it (none where a clip holds it but no probe sees it), else the environment's (indirect); and
// the environment's specular as much as the probes see of it, their light along the reflection as it gets rough
// (specularRadiance)
void TracedIndirect(float3 position, float3 normal, float3 direction, float roughness, out float3 indirect, out float3 specularRadiance)
{
	EnvironmentData environment = gEnvironment[0];
	float3 reflected = DominantDirection(normal, reflect(direction, normal), roughness);
	specularRadiance = EnvironmentRadiance(environment, reflected, roughness);
	GiSample sample = GiSampleSurface(position, normal, -direction);
	indirect = sample.valid ? GiEvaluate(sample, normal) : (sample.inside ? 0.0 : -1.0);
	if (indirect.x >= 0.0)
	{
		float3 luminance = float3(0.2126, 0.7152, 0.0722);
		float seen = saturate(dot(indirect, luminance) / max(dot(EnvironmentIrradiance(environment, normal), luminance), 1e-4));
		specularRadiance = lerp(specularRadiance * seen, GiEvaluate(sample, reflected), smoothstep(0.2, 0.9, roughness));
	}
	else
		indirect = EnvironmentIrradiance(environment, normal);
}

// the radiance along a ray from origin: what it meets of the acceleration structures (what moves too, alpha masks cut
// out), shaded, else the environment along it, prefiltered for the roughness of the surface it leaves (as Shade looks
// it up: the sky's sun, and a panorama's dominant lights, only where that is under the first prefiltered level's, see
// EnvironmentRadiance; a rougher surface's highlight is theirs as lights, and a glossy ray meeting the sun's disk took
// it twice, a sample thousands of times the sky's). cone: the ray's at origin. t: how far it met something
// (kTracedMissDistance where nothing). valid: false where it met a single sided triangle's back, which says it started
// inside something or looks where the triangle isn't drawn from, so what it would show isn't known here: the callers keep
// what the shading would have taken without it. shaded, such a hit was black (from inside, every light's shadowed and the
// probes see nothing): USDShaderBallForGltf's glass shell, its refraction leaving at its thickness inside the core.
// moving: whether what it met moves (what moves' structure: no model instance).
//
// a transmissive surface that doesn't move gives its reflection of what is around it, as a hit's specular is taken
// (TracedIndirect: the environment as much as the probes see of it; not its own traced reflections), and the light
// through it (TracedGlassPath, as the traced refractions take it), from inside its volume out of it (what is in the glass,
// a mosquito in amber, reflecting); valid as the path's. the opaque scene's structures hold no glass, and the rays passed
// through it: glass was missing from every reflection, CausticSpheres' amber sphere from its clear sphere's
float3 TracedRadiance(float3 origin, float3 direction, TracedCone cone, float roughness, out float t, out bool valid, out bool moving)
{
	bool glass = false;
	BvhHit hit;
	if ((kSkippedLayers & SHADER_TYPES_LAYERS_GLASS) != 0u)
		hit = BvhTraceScene(origin, direction, kTracedFar, false, true, GiAlphaMask());
	else
		hit = BvhTraceSceneOrGlass(origin, direction, kTracedFar, GiAlphaMask(), glass);
	if (!glass)
		return TracedShade(hit, origin, direction, cone, roughness, t, valid, moving);
	t = hit.t;
	uint frameIndex = gPushConstants.frameIndex;
	GlassSurface surface = GlassSurfaceAt(origin, direction, hit, frameIndex);
	GlassPath path;
	path.origin = origin;
	path.direction = direction;
	path.throughput = 1.0;
	path.inside = surface.material.transmission.y > 0.0 && !hit.frontFace;
	float3 reflection = 0.0;
	if (!path.inside)
	{
		float glassRoughness = surface.material.roughness;
		if ((surface.material.flags & MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE) != 0u)
		{
			TextureView textureView = gTextureViews[surface.material.metallicRoughnessView];
			glassRoughness *= SampleViewLevel(textureView, ViewTexCoord(textureView, surface.texCoord01), 0.0).r;
		}
		float3 indirect;
		float3 specularRadiance;
		TracedIndirect(surface.position, surface.normal, direction, glassRoughness, indirect, specularRadiance);
		float nDotV = max(dot(surface.normal, -direction), 1e-4);
		reflection = EnvironmentBrdfApprox(float3(surface.f0), 1.0, glassRoughness, nDotV) * specularRadiance;
	}
	moving = false;
	valid = true;
	if (!GlassPathStep(path, hit, frameIndex))
		return reflection;
	float3 through = TracedGlassPath(path, cone, hit.t, roughness, kTracedReflectionGlassSurfaceCount, frameIndex, valid, moving);
	return reflection + through;
}

// the light along a path through the glass (see GlassPath) from where it is, through the transmissive surfaces that don't
// move, to what it meets of the opaque scene (TracedShade) or the environment (prefiltered for roughness), less what the
// surfaces keep. cone: the ray's where it started, traveled along the path to its origin. valid: as TracedRadiance's, and
// false where its last of surfaceCount queries met glass still. moving: as TracedRadiance's
float3 TracedGlassPath(
	GlassPath path, TracedCone cone, float traveled, float roughness, uint surfaceCount, uint frameIndex, out bool valid, out bool moving)
{
	valid = false;
	moving = false;
	for (uint surfaceIt = 0; surfaceIt < surfaceCount; surfaceIt++)
	{
		bool glass;
		BvhHit hit = BvhTraceSceneOrGlass(path.origin, path.direction, kTracedFar, GiAlphaMask(), glass);
		if (!glass)
		{
			TracedCone at = cone;
			at.width += cone.spread * traveled;
			float t;
			return path.throughput * TracedShade(hit, path.origin, path.direction, at, roughness, t, valid, moving);
		}
		traveled += hit.t;
		if (!GlassPathStep(path, hit, frameIndex))
		{
			valid = true;
			return 0.0;
		}
	}
	return 0.0;
}

// what TracedRadiance gives of a ray's hit (or of nothing met)
float3 TracedShade(BvhHit hit, float3 origin, float3 direction, TracedCone cone, float roughness, out float t, out bool valid, out bool moving)
{
	valid = true;
	moving = false;
	if (!BvhHitSomething(hit))
	{
		t = kTracedMissDistance;
		return EnvironmentRadiance(gEnvironment[0], direction, roughness);
	}
	t = hit.t;
	moving = hit.modelInstance == ~0u;
	if (!hit.frontFace && (hit.flags & BVH_TRIANGLE_FLAG_DOUBLE_SIDED) == 0u)
	{
		valid = false;
		return 0.0;
	}
	return TracedHitRadiance(direction, hit, origin + direction * hit.t, cone.width + cone.spread * hit.t);
}

// a reflection a normal map turned below the geometry, turned to skim it instead (see TracedReflection)
float3 TracedAboveGeometry(float3 direction, float3 geometryNormal)
{
	float below = dot(direction, geometryNormal) - 0.02;
	return below < 0.0 ? normalize(direction - geometryNormal * below) : direction;
}

// the radiance a surface at position reflects along direction from (its reflection's dominant direction, see Shade, or
// a direction of its lobe, see TracedGlossyReflection, whose width widens the cone: spread). the ray starts off the
// surface on the side it goes to (the geometry's, facing the viewer), by a pixel and a half and a ten thousandth of the
// distance from the eye (the depth's and the structures' rounding), as the traced shadows' rays do; a normal map that
// turns it below the geometry has it skim the surface instead, which it would otherwise meet at once. roughness, t,
// valid, moving: as TracedRadiance's
float3 TracedReflection(
	float3 position, float3 geometryNormal, float3 direction, float spread, float roughness, out float t, out bool valid, out bool moving)
{
	TracedCone cone = TracedPixelCone(position);
	cone.spread += spread;
	direction = TracedAboveGeometry(direction, geometryNormal);
	float3 eye = gViewData[gPushConstants.frameIndex][gShadingViewId].eyePosition.xyz;
	float push = max(1.5 * cone.width, 1e-4 * distance(eye, position));
	return TracedRadiance(position + geometryNormal * push, direction, cone, roughness, t, valid, moving);
}

float3 TracedReflection(float3 position, float3 geometryNormal, float3 direction, out bool valid)
{
	float t;
	bool moving;
	return TracedReflection(position, geometryNormal, direction, 0.0, 0.0, t, valid, moving);
}

// a pixel's point of the unit square for the frame: Roberts' R2 sequence over the frames, offset per pixel by interleaved
// gradient noise (each coordinate its own), so that neighbors sample other directions and each pixel all of them in turn
float2 TracedRandom(uint2 pixel)
{
	float2 at = float2(pixel);
	float2 offset = float2(
		frac(52.9829189 * frac(dot(at, float2(0.06711056, 0.00583715)))),
		frac(52.9829189 * frac(dot(at + 17.0, float2(0.00583715, 0.06711056)))));
	return frac(offset + float(TracedFrame() & 0xffffu) * float2(0.7548776662, 0.5698402910));
}

// a microfacet normal of the GGX lobe of alpha about +z, by the normals visible from view (in the same frame, z up):
// Heitz, "Sampling the GGX Distribution of Visible Normals" (2018). u: a point of the unit square
float3 TracedVisibleNormal(float3 view, float alpha, float2 u)
{
	float3 stretched = normalize(float3(alpha * view.x, alpha * view.y, view.z));
	float lengthSquared = stretched.x * stretched.x + stretched.y * stretched.y;
	float3 t1 = lengthSquared > 0.0 ? float3(-stretched.y, stretched.x, 0.0) * rsqrt(lengthSquared) : float3(1.0, 0.0, 0.0);
	float3 t2 = cross(stretched, t1);
	float r = sqrt(u.x);
	float phi = 6.28318530718 * u.y;
	float p1 = r * cos(phi);
	float p2 = r * sin(phi);
	float s = 0.5 * (1.0 + stretched.z);
	p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
	float3 normal = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * stretched;
	return normalize(float3(alpha * normal.x, alpha * normal.y, max(normal.z, 1e-6)));
}

// the visibility buffer's glossy reflection at its pixel (see Shade): a ray along view reflected about a normal of the
// GGX lobe of the roughness (TracedVisibleNormal; the mirror direction where that falls below the surface), its radiance
// and how far it met something: one sample, which the Reflection passes filter over the surface and over frames, and the
// average of which is the lobe's (what the split sum's first term prefilters the environment by), its distance packed
// with whether what it met moves (ReflectionPackDistance). valid: as TracedRadiance's. direction: the ray's
float4 TracedGlossyReflection(
	float3 position, float3 geometryNormal, float3 normal, float3 view, float roughness, uint2 pixel, out float3 direction, out bool valid)
{
	float alpha = max(roughness * roughness, 1e-3);
	float3 tangent = normalize(abs(normal.z) < 0.999 ? cross(float3(0.0, 0.0, 1.0), normal) : cross(float3(1.0, 0.0, 0.0), normal));
	float3 bitangent = cross(normal, tangent);
	float3 local = normalize(float3(dot(view, tangent), dot(view, bitangent), max(dot(view, normal), 1e-4)));
	float3 h = TracedVisibleNormal(local, alpha, TracedRandom(pixel));
	direction = reflect(-view, h.x * tangent + h.y * bitangent + h.z * normal);
	if (dot(direction, normal) <= 0.0)
		direction = reflect(-view, normal);
	// as traced, for what is predicted along it (see Shade): along the direction below the geometry, the prediction saw
	// the ground where the ray skimming it saw the sky, and the edges of Bistro's lamp posts and scooter came out bright
	direction = TracedAboveGeometry(direction, geometryNormal);
	float t;
	bool moving;
	float3 radiance = TracedReflection(position, geometryNormal, direction, alpha, roughness, t, valid, moving);
	// a sample far brighter than white at the view's exposure is taken as a few times that: one that met what is very
	// bright (a lamp's bulb, the sun's glint off a mirror), which the filter spread into a square, alone among its
	// neighbors'; tonemapped, they look the same
	float exposure = gViewData[gPushConstants.frameIndex][gShadingViewId].exposure;
	float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
	float most = kReflectionMostWhite / max(exposure, 1e-6);
	if (luminance > most)
		radiance *= most / luminance;
	return float4(radiance, ReflectionPackDistance(min(t, kTracedMissDistance), moving));
}

// the light through a transmissive surface at position, from the surface along direction (into its volume, refracted:
// volume; through a thin wall, as the view goes), through the transmissive surfaces that don't move as the caustics'
// photons go (TracedGlassPath, GlassPathStep: out of the volume where it meets its back, refracted, or reflected back in
// where it can't leave; into or through the glass beyond), to what it meets of the opaque scene, less what the surfaces
// past this one keep (this one's own attenuation and Fresnel are the caller's). the opaque scene's structures hold no
// glass, and the ray used to pass through its own volume without leaving it: a ball showed what is behind it shifted
// rather than turned upside down, CausticSpheres' clear sphere the ground beyond its shadow, lit. a transmissive surface
// that moves (no structure holds it) is passed so still. from the surface, not from where the screen space lookup has
// the ray leave the volume (at the material's thickness along the ray, see TransmittedLight): USDShaderBallForGltf's
// glass shell has a thickness of up to 8.9 in a model a tenth of a unit across, which put the exit inside the core (its
// back met: black) or past everything (the environment's), a wavy border between the two. valid: as TracedGlassPath's
float3 TracedRefraction(float3 position, float3 direction, bool volume, out bool valid)
{
	TracedCone cone = TracedPixelCone(position);
	float3 eye = gViewData[gPushConstants.frameIndex][gShadingViewId].eyePosition.xyz;
	float push = max(cone.width, 1e-4 * distance(eye, position));
	GlassPath path;
	path.origin = position + direction * push;
	path.direction = direction;
	path.throughput = 1.0;
	path.inside = volume;
	bool moving;
	return TracedGlassPath(path, cone, push, 0.0, SHADER_TYPES_CAUSTIC_SURFACE_COUNT, gPushConstants.frameIndex, valid, moving);
}
#endif

// the blur a glossy reflection's lobe makes at a pixel, in pixels: its width where the rays meet something (the hit
// distance by the lobe's angle, about alpha) over the pixel's width on the surface, at most 16 (the filter's reach)
float ReflectionBlurPixels(float roughness, float hitDistance, float metersPerPixel)
{
	return min(hitDistance * roughness * roughness / max(metersPerPixel, 1e-6), 16.0);
}

// a pass of the glossy reflections' filter: the rays' shares and hit distance (or the rows' result) averaged along the
// row (or column) over the blur the pixel's lobe makes there (ReflectionBlurPixels, by its roughness and its own sample's
// hit distance), by a gaussian, weighed by how much each tap is on the pixel's surface (as the near field's, see
// GiNearFilter) and of its roughness, leaving out what wasn't traced. a mirror's (under half a pixel) is its own
float4 ReflectionFilter(int2 pixel, int2 step, uint source)
{
	// what wasn't traced first: most of a frame, whose surface needn't be found
	Texture2D weights = gTextures[SHADER_TYPES_REFLECTION_WEIGHT_TEXTURE];
	Texture2D input = gTextures[source];
	float roughness = weights.Load(int3(pixel, 0)).a;
	float4 own = input.Load(int3(pixel, 0));
	if (roughness < 0.0 || ReflectionUnknown(own.a))
		return kReflectionUnknown;
	roughness = ReflectionRoughness(roughness);
	ViewData view;
	ShadowSurface center;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, center))
		return kReflectionUnknown;
	float radius = ReflectionBlurPixels(roughness, ReflectionDistance(own.a), center.metersPerPixel);
	if (radius < 0.5)
		return own;
	int reach = int(ceil(radius));
	float sigma = max(0.5 * radius, 0.5);
	float2 low = view.viewport.xy;
	float2 high = view.viewport.xy + view.viewport.zw;
	float3 total = 0.0;
	float hitDistance = 0.0;
	float moving = 0.0;
	float sum = 0.0;
	for (int offset = -reach; offset <= reach; offset++)
	{
		int2 at = pixel + step * offset;
		if (any(float2(at) < low) || any(float2(at) >= high))
			continue;
		float4 value = input.Load(int3(at, 0));
		float tapRoughness = weights.Load(int3(at, 0)).a;
		if (ReflectionUnknown(value.a) || tapRoughness < 0.0)
			continue;
		tapRoughness = ReflectionRoughness(tapRoughness);
		float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(at, 0)).r;
		if (depth <= 0.0)
			continue;
		float3 position = ShadowPosition(view, float2(at), depth);
		float3 normal = gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(at, 0)).xyz * 2.0 - 1.0;
		float plane = abs(dot(position - center.position, center.normal)) / (2.0 * center.metersPerPixel);
		float facing = saturate(dot(normal, center.normal) * rsqrt(max(dot(normal, normal), 1e-8)));
		float w = exp(-0.5 * float(offset * offset) / (sigma * sigma)) * exp(-plane) * pow(facing, 8.0) *
				  exp(-20.0 * abs(tapRoughness - roughness));
		total += w * value.rgb;
		hitDistance += w * ReflectionDistance(value.a);
		moving += ReflectionMoving(value.a) ? w : 0.0;
		sum += w;
	}
	return sum > 0.0 ? float4(total / sum, ReflectionPackDistance(hitDistance / sum, moving > 1e-3 * sum)) : own;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeReflectionX(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 extent;
	gRWTextures[SHADER_TYPES_REFLECTION_ROWS_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(pixel >= int2(extent)))
		return;
	gRWTextures[SHADER_TYPES_REFLECTION_ROWS_RW_TEXTURE][pixel] = ReflectionFilter(pixel, int2(1, 0), SHADER_TYPES_REFLECTION_RAW_TEXTURE);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeReflectionY(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 extent;
	gRWTextures[SHADER_TYPES_REFLECTION_FILTERED_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(pixel >= int2(extent)))
		return;
	gRWTextures[SHADER_TYPES_REFLECTION_FILTERED_RW_TEXTURE][pixel] = ReflectionFilter(pixel, int2(0, 1), SHADER_TYPES_REFLECTION_ROWS_TEXTURE);
}

// the glossy reflections' history where what a pixel reflects was seen last frame (virtual motion, as NVIDIA's ReBLUR has
// it): at the image of what the reflection met, the hit distance behind the surface along the view, through the view's
// last view projection, which is exact for a flat mirror; bilinear over the 4 pixels there, each only if its surface faced
// as this one's does and what its reflection met was about as far (else the image was another surface's, or of something
// else)
float4 ReflectionVirtualHistory(
	Texture2D last, Texture2D lastSurfaces, ViewData view, float2 at, float3 normal, float hitDistance, out float weights, out float held)
{
	int2 base = int2(floor(at));
	float2 f = at - float2(base);
	float4 total = 0.0;
	weights = 0.0;
	held = 0.0;
	for (int corner = 0; corner < 4; corner++)
	{
		int2 offset = int2(corner & 1, corner >> 1);
		int2 tap = base + offset;
		if (any(float2(tap) < view.viewport.xy) || any(float2(tap) >= view.viewport.xy + view.viewport.zw))
			continue;
		float4 value = last.Load(int3(tap, 0));
		float3 seen = lastSurfaces.Load(int3(tap, 0)).xyz;
		if (value.a < 0.0 || seen.x < 0.0 || dot(EmitterUnpackNormal(seen.y), normal) < 0.95 || abs(value.a - hitDistance) > 0.2 * hitDistance)
			continue;
		float w = (offset.x != 0 ? f.x : 1.0 - f.x) * (offset.y != 0 ? f.y : 1.0 - f.y);
		total += w * value;
		weights += w;
		held += w * seen.z;
	}
	return total;
}

// a surface's curvature at a pixel, signed (convex positive, 1 over its radius): how its normal turns along the surface
// toward the pixels beside it on the same surface (not across a step: one off its tangent plane by more than 30 degrees)
float ReflectionCurvature(int2 pixel, ViewData view, ShadowSurface center)
{
	float total = 0.0;
	float count = 0.0;
	for (int side = 0; side < 4; side++)
	{
		int2 at = pixel + (side < 2 ? int2(side * 2 - 1, 0) : int2(0, side * 2 - 5));
		ShadowSurface beside;
		if (any(float2(at) < view.viewport.xy) || any(float2(at) >= view.viewport.xy + view.viewport.zw) || !ShadowSurfaceAt(at, view, beside))
			continue;
		float3 along = beside.position - center.position;
		float lengthSquared = dot(along, along);
		if (lengthSquared <= 0.0 || abs(dot(along, center.normal)) > 0.5 * sqrt(lengthSquared))
			continue;
		total += dot(beside.normal - center.normal, along) / lengthSquared;
		count += 1.0;
	}
	return count > 0.0 ? total / count : 0.0;
}

// the glossy reflections over frames, and into the main color: the frame's filtered shares (see Shade) blended into the
// last frame's history where the pixel's point was (PixelHistory, as the near field's: by its surface, through the view's last
// view projection, none for what moves), clamped to the box of the frame's 3x3 around as far as what the surface reflects
// slid across it (as the eye travels, which reprojecting the surface doesn't follow: kept, it trailed), by 1 over the
// frames it holds, from kReflectionFramesSharp at a mirror to kReflectionFramesRough (kReflectionFramesStill where nothing
// slid); then the specular's weight of it
// added to the main color (see Shade, which left that share out), before the passes that draw over it or read it
// (Main's blended surfaces, the transmission texture)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeReflectionTemporal(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint current = TracedFrame() & 1u;
	RWTexture2D history = gRWTextures[SHADER_TYPES_REFLECTION_HISTORY_RW_TEXTURE + current];
	RWTexture2D surfaces = gRWTextures[SHADER_TYPES_REFLECTION_SURFACE_RW_TEXTURE + current];
	uint2 extent;
	history.GetDimensions(extent.x, extent.y);
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(pixel >= int2(extent)))
		return;
	ViewData view;
	ShadowSurface surface;
	float4 weight = gTextures[SHADER_TYPES_REFLECTION_WEIGHT_TEXTURE].Load(int3(pixel, 0));
	Texture2D filtered = gTextures[SHADER_TYPES_REFLECTION_FILTERED_TEXTURE];
	float4 now = filtered.Load(int3(pixel, 0));
	if (weight.a < 0.0 || ReflectionUnknown(now.a) || !ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, surface))
	{
		history[pixel] = kReflectionUnknown;
		surfaces[pixel] = float4(-1.0, 0.0, 0.0, 0.0);
		return;
	}
	// the history holds the distance alone (PixelHistory takes a negative alpha for none)
	bool movingSeen = ReflectionMoving(now.a);
	now.a = ReflectionDistance(now.a);
	float roughness = ReflectionRoughness(weight.a);
	bool bumpy = ReflectionBumpy(weight.a);
	float3 low = now.rgb;
	float3 high = now.rgb;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			int2 at = clamp(pixel + int2(x, y), int2(0), int2(extent) - 1);
			float4 value = filtered.Load(int3(at, 0));
			if (ReflectionUnknown(value.a))
				continue;
			low = min(low, value.rgb);
			high = max(high, value.rgb);
			bumpy = bumpy || ReflectionBumpy(gTextures[SHADER_TYPES_REFLECTION_WEIGHT_TEXTURE].Load(int3(at, 0)).a);
		}
	float3 toEye = view.eyePosition.xyz - surface.position;
	float count = 1.0; // the frames the history holds
	float4 result = now;
	bool moves = (gMainStencil.Load(int3(pixel, 0)) & 1u) != 0u;
	if ((gPushConstants.traced & TRACED_REFLECTION_HISTORY) != 0u && view.previousEyePosition.w > 0.0 && !moves)
	{
		Texture2D lastHistory = gTextures[SHADER_TYPES_REFLECTION_HISTORY_TEXTURE + (current ^ 1u)];
		Texture2D lastSurfaces = gTextures[SHADER_TYPES_REFLECTION_SURFACE_TEXTURE + (current ^ 1u)];
		// how far what the surface reflects slid across it since the last frame, by the blur its lobe makes there: the eye
		// travelling dt, the image of what the reflection met (hit distance t, behind the surface at distance d) slides
		// against the surface by dt t / (d + t) (a turn of the view about the eye slides nothing: the reflection is the eye's
		// place's), in pixels over the pixel's width there. what slid by less than its blur is the history's still; by more,
		// it is clamped to the frame's box, which a slide trailed without (a mirror's far reflection). clamped wherever the
		// view moved, ChronographWatch's strap under small turns of the view was as noisy as a frame (a mean second
		// difference of 4.49 of 255 over the frames, untraced 0.60)
		float travel = EmitterViewStill(view) ? 0.0 : distance(view.eyePosition.xyz, view.previousEyePosition.xyz);
		float hitDistance = now.a;
		float slide = travel * hitDistance / ((length(toEye) + hitDistance) * max(surface.metersPerPixel, 1e-6)) /
					  max(ReflectionBlurPixels(roughness, hitDistance, surface.metersPerPixel), 1.0);
		bool settled = (gPushConstants.traced & TRACED_SETTLED) != 0u && !movingSeen;
		float clamping = settled ? saturate(slide) : 1.0;
		float4 total = 0.0;
		float weights = 0.0;
		float held = 0.0;
		float4 clip = mul(view.previousViewProjection, float4(surface.position, 1.0));
		if (clip.w > 0.0)
		{
			float expected = distance(surface.position, view.previousEyePosition.xyz);
			float slant = abs(dot(surface.normal, normalize(toEye)));
			float tolerance = 0.002 * expected + 2.0 * surface.metersPerPixel * sqrt(max(1.0 - slant * slant, 0.0)) / max(slant, 0.05);
			total = PixelHistory(lastHistory, lastSurfaces, view, PixelHistoryAt(view, clip), expected, tolerance, surface.normal, weights, held);
		}
		// where it slid, the history where it was seen last frame instead (ReflectionVirtualHistory), unclamped: clamped,
		// a step aside close to the watch's case slid the strap's reflection across it by tens of pixels a frame, and it
		// was as noisy as a frame again (a mean second difference over the frames of 0.92 of 255, untraced 0.09, now 0.18).
		// the image is as deep behind the surface as a curved mirror puts it (1 / v = 1 / t + 2 k, k the curvature): as a
		// flat mirror's, MetalRoughSpheresNoTextures' glossy spheres smeared their reflections as the eye walked, the image
		// of what a convex mirror reflects being nearer (half its radius deep, of what is far). none where a concave surface
		// focuses it in front of itself, nor where a normal map turns the normal (ReflectionPackRoughness): each cell of
		// ChronographWatch's woven strap reflects along its own direction, the image of a mirror was another cell's, and it
		// trailed along the move (its last frame 3.68 of 255 from a still render of its pose, 2.09 clamped)
		float focus = clamping > 0.0 && settled && !bumpy ? 1.0 + 2.0 * ReflectionCurvature(pixel, view, surface) * hitDistance : 0.0;
		if (focus > 0.25)
		{
			float depth = hitDistance / focus;
			float4 clipImage = mul(view.previousViewProjection, float4(surface.position - normalize(toEye) * depth, 1.0));
			float imageWeights;
			float imageHeld;
			float4 imageTotal = clipImage.w > 0.0 ? ReflectionVirtualHistory(lastHistory, lastSurfaces, view, PixelHistoryAt(view, clipImage),
																			 surface.normal, hitDistance, imageWeights, imageHeld)
												 : 0.0;
			if (clipImage.w > 0.0 && imageWeights > 1e-3)
			{
				total = imageTotal;
				weights = imageWeights;
				held = imageHeld;
				clamping = 0.0;
			}
		}
		if (weights > 1e-3)
		{
			float4 kept = total / weights;
			kept.rgb = lerp(kept.rgb, clamp(kept.rgb, low, high), clamping);
			float frames = lerp(kReflectionFramesStill, lerp(kReflectionFramesSharp, kReflectionFramesRough, smoothstep(0.05, 0.5, roughness)), clamping);
			count = min(max(held / weights, 1.0) + 1.0, frames);
			result = lerp(kept, now, 1.0 / count);
		}
	}
	history[pixel] = result;
	surfaces[pixel] = float4(length(toEye), EmitterPackNormal(surface.normal), count, 0.0);
	float4 color = gMainColor[pixel];
	gMainColor[pixel] = float4(color.rgb + weight.rgb * result.rgb, color.a);
}
