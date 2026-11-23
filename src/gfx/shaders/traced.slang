// the traced reflections and refractions (PushConstants::traced, TRACED_*; View > Rendering Features > Ray Traced Reflections and
// Refractions): where the device has ray queries (BVH_RAY_QUERY), the radiance along a smooth surface's reflection
// (TracedReflection, see Shade) and the light through a transmissive one (see TransmittedLight), traced
// through the model's acceleration structures (gBvhScene[frame], BvhTraceScene) and shaded where the ray meets something
// (TracedHitRadiance), the environment where it meets nothing. a fragment's (Main's blended surfaces, the transmissive
// ones, the forward path) sharp: a ray along the reflection's dominant direction (or the refracted ray), all of the
// reflection below kTracedRoughnessFull, faded into the glossy one the probes and the environment give (or the
// transmission texture's mips) by kTracedRoughnessEnd. the visibility buffer's glossy: a ray a pixel of the GGX lobe
// (TracedGlossyDirection), up to kTracedGlossyRoughnessEnd, which its shading records and the Reflection Trace pass
// traces (ComputeReflectionTrace, with the clearcoat's), filtered over the surface and over frames by the Reflection
// passes (ComputeReflectionX, Y and Temporal), which add it to the main color. a part of shaders.slang, which includes it
// after capi.h and sky.h

// the roughness up to which a reflection (or a transmission's blur, see TransmittedLight) is all traced, and from which
// none of it is: a ray along the dominant direction is a mirror's reflection, too sharp for a glossy surface
static const float kTracedRoughnessFull = 0.15;
static const float kTracedRoughnessEnd = 0.4;
// how far the rays go, in the scene's units (any scene's: what they meet is nearer)
static const float kTracedFar = 1.0e30;
// the forward path's sharp rays (Main's blended and transmissive surfaces, every surface without the visibility buffer),
// traced in one loop (TracedFragmentRays): a fragment's base and clearcoat reflections (see Shade), then its refraction
// (see TransmittedLight), a ray for each color where dispersion refracts each by its own ior
static const uint kTracedFragmentBase = 0u;
static const uint kTracedFragmentCoat = 1u;
static const uint kTracedFragmentRefraction = 2u;
static const uint kTracedFragmentRayCount = 5u;
// how many transmissive surfaces a reflection's ray goes through (see TracedRadiance), the one it met first among them:
// past them the caller keeps what it would have taken untraced
static const uint kTracedReflectionGlassSurfaceCount = 2u;
// the emitters' highlights at a traced hit (see TracedHitRadiance): sampled where the hit's specular takes at least this
// share of the light (the split sum's terms, as luminance: a dielectric's 0.04 at normal incidence, a metal's most of
// it), from this many candidates, one shadow ray
static const float kTracedEmitterSpecularLeast = 0.1;
static const uint kTracedEmitterCandidateCount = 2u;
// the candidates a glossy reflection's light sample is resampled from (see TracedGlossyEmitters: one, the tree's pick
// alone; two cost Bistro at night 0.17 ms a frame more at 1440p and measured the same, settled and at 8 frames), and
// below how much of white at the view's exposure (of what the pixel takes of it) its shadow ray is traced only at random,
// as often as its share of this (Russian roulette: what it gives, divided by that chance, is the same on average; the ray
// traced always cost Bistro at night 0.3 ms more)
static const uint kTracedGlossyEmitterCandidateCount = 1u;
static const float kTracedGlossyEmitterTracedLeast = 0.25;

// what the last traced path's hit emits toward it (TracedHitRadiance's emission, less what the glass on the way kept: see
// TracedPath), where it is, and whether the emitters' light samples could have picked it (a hit of what doesn't move, where
// emitters are lights; see emitters::Build): what the Reflection Trace pass weighs against its light sample
static float3 gTracedHitEmission;
static float3 gTracedHitPoint;
static bool gTracedHitEmitter;
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
// GGX lobe, filtered over the surface and over frames, see TracedGlossyDirection), and from which none of them: rougher,
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
// the brightest light a pixel takes of a glossy ray's sample (by its weight), in multiples of white at the view's exposure
// (see ComputeReflectionTrace)
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
// passes, rather than a fragment's (sharp: see TracedFragmentRays)
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

// the visibility buffer's shading's glossy reflection at a pixel, which the Reflection Trace pass traces (see
// ComputeReflectionTrace): the ray's direction and the normal its lobe is about (octahedral, 32 bits each: the light
// sample's weight against the ray takes the lobe's density, which a mirror's narrow lobe makes sensitive to a normal's
// rounding, see TracedGlossyEmitters), and what the probes and the environment predict along it (expected, where the
// trace writes the ray's radiance as a share of it and its hit distance), and the specular's weight of the pixel's own
// prediction with the surface's roughness (see Shade; ComputeReflectionTemporal adds the filtered share's product with it
// to the main color); kReflectionUnknown in both where none is traced. in compute only: a fragment shader's storage
// writes turn its early depth test off, even on a path never taken (see GiMarch's cap)
void TracedReflectionRecord(uint2 pixel, float3 direction, float3 normal, float4 expected, float4 weight)
{
	__stage_switch
	{
	case compute:
		gRWTextures[SHADER_TYPES_REFLECTION_RAW_RW_TEXTURE][pixel] = expected;
		gRWTextures[SHADER_TYPES_REFLECTION_WEIGHT_RW_TEXTURE][pixel] = weight;
		gRWTextures[SHADER_TYPES_REFLECTION_RAY_RW_TEXTURE][pixel] = float4(GiOctahedral(direction), GiOctahedral(normal));
		return;
	case fragment:
		return;
	}
}

// and its clearcoat's reflection, which the pass traces sharp, as a fragment's (see Shade): the coat's dominant direction
// and roughness (the environment's prefiltering, which the trace stands in for), and the weight of the difference the
// trace makes to the coat's light (0 where none is traced)
void TracedCoatRecord(uint2 pixel, float3 direction, float roughness, float weight)
{
	__stage_switch
	{
	case compute:
		gRWTextures[SHADER_TYPES_REFLECTION_COAT_RW_TEXTURE][pixel] = float4(GiOctahedral(direction), roughness, weight);
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
		emission *= EmissiveTexel(TracedSample(material.emissiveView, texCoords, weights, worldArea, coneWidth).rgb);
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
	gTracedHitEmission = emission;
	gTracedHitPoint = position;
	gTracedHitEmitter = hit.modelInstance != ~0u && any(emission > 0.0) && EmittersLight();

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
	// the emissive surfaces' light. the diffuse: where the hit is on the screen, what the main pass gathered at its pixel
	// (TracedEmitterSeen: filtered and over frames), else what the hit's voxel keeps (TracedEmitterKept). the specular
	// (their highlights), where the hit's specular takes at least kTracedEmitterSpecularLeast of the light (metals, and
	// what grazing angles make a mirror of): a point resampled from candidates down the light tree (EmitterCandidates) and
	// its shadow ray (EmitterShade), by the specular lobe. without, what the emitters light was lit by the probes alone
	// wherever a ray met it, and the glTF research set's glass-of-water, whose backdrop and floor are rough metals an
	// emissive quad lights, was black through the glass, 2.9 stops off its reference (docs/research-scenes-plan.md, B1). a
	// light sample at every hit cost Bistro at night 3.1 ms a frame at 1440p (2.2 with one candidate, not four), and the
	// lookup on the screen first saved little: a wave waits for its slowest thread
	if (EmittersLight())
	{
		// (none for a metal, which has no diffuse to take it). where the screen hides the hit while the emitters' caustics
		// are traced, the caustics' cells and a light sample (TracedEmitterHidden): the voxels' light is the emitters'
		// straight through the glass, which the caustics take the place of
		if (any(surface.diffuseColor > 0.0))
		{
			bool hidden;
			float3 seen = TracedEmitterSeen(position, normal, hidden);
			if (seen.x >= 0.0)
				radiance += surface.diffuseColor * seen;
			else if (hidden && EmitterCausticsTraced())
			{
				float nearest = max(coneWidth, push);
				radiance += surface.diffuseColor * TracedEmitterHidden(position, normal, hit.normal, push, nearest * nearest);
			}
			else
				radiance += TracedEmitterKept(surface, view, position, normal, hit);
		}
		float3 specularShare = EnvironmentBrdfApprox(surface.f0, surface.f90, surface.roughness, nDotV);
		if (dot(specularShare, float3(0.2126, 0.7152, 0.0722)) >= kTracedEmitterSpecularLeast)
		{
			EmitterRandom random = EmitterRandomAt(gFragmentPixel ^ uint2(0x2545u, 0xf491u), gEmitterData[frameIndex][0].frame);
			float nearest = max(coneWidth, push);
			EmitterReservoir reservoir = EmitterCandidates(position, normal, kTracedEmitterCandidateCount, nearest * nearest, random);
			bool visible;
			bool throughGlass; // (the highlights keep the light straight through glass, see EmitterGatheredByGlass)
			float3 irradiance = EmitterShade(reservoir, position, normal, push, nearest * nearest, true, true, false, visible, throughGlass);
			if (visible)
			{
				Surface specular = surface;
				specular.diffuseColor = 0.0;
				float3 toPoint = normalize(EmitterPointAt(reservoir.triangle, reservoir.barycentrics).position - position);
				radiance += BaseLobe(specular, view, toPoint, max(dot(normal, toPoint), 1e-4)) * irradiance;
			}
		}
	}

	float3 indirect;
	float3 specularRadiance;
	TracedIndirect(position, normal, direction, surface.roughness, hit.normal, push, indirect, specularRadiance);
	radiance += surface.diffuseColor * indirect +
				EnvironmentBrdfApprox(surface.f0, surface.f90, surface.roughness, nDotV) * specularRadiance;
	return radiance;
}

// the indirect lighting at a hit at position (normal, met along direction, of a roughness), as Shade takes it: the probes'
// diffuse where they cover it (none where a clip holds it but no probe sees it), else the environment's (indirect); and
// the environment's specular as much as the probes see of it, their light along the reflection as it gets rough
// (specularRadiance). where the lookup barely sees its probes (GiLookupTrust), the light of near field rays of the hit's
// own (TracedNearLight: from its surface, nearNormal toward the ray, off it by nearBias; 0 for none) stands in for its
// second try, as the near field's does for the main pass's fragments (see GiNearIrradiance): the floor in front of
// Sponza's curtains, lit by that try from the courtyard beyond them, was the light the gold strip at their foot reflected
// in orange flames and blocks
void TracedIndirect(
	float3 position, float3 normal, float3 direction, float roughness, float3 nearNormal, float nearBias, out float3 indirect,
	out float3 specularRadiance)
{
	EnvironmentData environment = gEnvironment[0];
	float3 reflected = DominantDirection(normal, reflect(direction, normal), roughness);
	specularRadiance = EnvironmentRadiance(environment, reflected, roughness);
	GiData gi = GiClip(0);
	float voxel = nearBias > 0.0 && gi.enabled != 0u && gi.nearReach > 0.0 && GiTracesBvh(gi) ? GiNearVoxelSize(position) : 0.0;
	// (the clipmap's light alone, not the world cache's: at the reflections' and the refractions' hits its lookups took
	// 0.6 and 0.9 ms more a frame in Bistro at night, for a light they show blurred; docs/world-cache-plan.md, step 5)
	bool moving;
	GiSample sample = GiSampleSurface(position, normal, -direction, GiSeenAt(position, normal), voxel <= 0.0, moving, false);
	float trust = voxel > 0.0 ? GiLookupTrust(sample) : 1.0;
	indirect = sample.valid ? GiEvaluate(sample, normal) : (sample.inside ? 0.0 : -1.0);
	if (indirect.x >= 0.0)
	{
		if (trust < 1.0)
			indirect = lerp(TracedNearLight(gi, position, nearNormal, nearBias, voxel, indirect), indirect, trust);
		float3 luminance = float3(0.2126, 0.7152, 0.0722);
		float seen = saturate(dot(indirect, luminance) / max(dot(EnvironmentIrradiance(environment, normal), luminance), 1e-4));
		specularRadiance = lerp(specularRadiance * seen, lerp(indirect, GiEvaluate(sample, reflected), trust), smoothstep(0.2, 0.9, roughness));
	}
	else
		indirect = EnvironmentIrradiance(environment, normal);
}

// the emitters' light where a traced hit is on the screen (see TracedHitRadiance): what the main pass gathered at the
// pixel there (Emitter Temporal's history, the irradiance over pi, as EmitterIrradiance takes it: filtered and over
// frames, no ray), where that pixel's surface is the hit's (Emitter Temporal's pixel surfaces: its distance from the eye
// within a hundredth, its normal within 25 degrees); -1 where the hit is off the screen or something else is there
// (hidden: on the screen, behind what the pixel shows, which a refraction or a reflection shows: see TracedEmitterHidden).
// most of a refraction's hits are what the view shows behind the glass, and a mirror's what it shows in front: their
// own sample each cost Bistro at night 2.2 ms a frame at 1440p
float3 TracedEmitterSeen(float3 position, float3 normal, out bool hidden)
{
	hidden = false;
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	ViewData view = gViewData[gPushConstants.frameIndex][gShadingViewId];
	float4 clip = mul(view.viewProjection, float4(position, 1.0));
	if (clip.w <= 0.0)
		return -1.0;
	float2 at = (clip.xy / clip.w * 0.5 + 0.5) * view.viewport.zw + view.viewport.xy;
	if (any(at < view.viewport.xy) || any(at >= view.viewport.xy + view.viewport.zw))
		return -1.0;
	int2 texel = int2(at);
	uint current = data.frame & 1u;
	float2 seen = gTextures[SHADER_TYPES_EMITTER_PIXEL_SURFACE_TEXTURE + current].Load(int3(texel, 0)).xy;
	float expected = distance(view.eyePosition.xyz, position);
	hidden = seen.x < 0.0 || abs(seen.x - expected) > 0.01 * expected || dot(EmitterUnpackNormal(seen.y), normal) < 0.9;
	if (hidden)
		return -1.0;
	float4 light = gTextures[SHADER_TYPES_EMITTER_HISTORY_TEXTURE + current].Load(int3(texel, 0));
	return light.a >= 0.0 ? light.rgb : -1.0;
}

// the emitters' light at a traced hit the screen hides (TracedEmitterSeen's hidden) while their caustics are traced: the
// caustics' from the cells of the world the photons land in (EmitterCausticCellLight; geometryNormal: the hit's, toward
// the ray), and a light sample of the rest, its shadow ray stopping at solid glass as the screen's diffuse's do
// (EmitterShade; nearest2: how near an emitter point counts, squared). the voxels' light (TracedEmitterKept), which it
// took before, is the emitters' straight through the glass: cornell-caustic's back wall beside its cubes, seen through
// the water's front face where the screen has the cube, was lit straight through the waves and shadowed by the cube as if
// the water weren't there (the user's report, 2026-11-23: "an additional light"); the light the screen gathered nearest
// it on its own plane in its place, rings of taps around its place, drew blocks of the wall's caustics from elsewhere
// among patches of the voxels' where no ring met the wall ("broken and corrupted"). the irradiance over pi
float3 TracedEmitterHidden(float3 position, float3 normal, float3 geometryNormal, float push, float nearest2)
{
	float3 light = EmitterCausticCellLight(position, geometryNormal);
	EmitterRandom random = EmitterRandomAt(gFragmentPixel ^ uint2(0x9e37u, 0x79b9u), gEmitterData[gPushConstants.frameIndex][0].frame);
	EmitterReservoir reservoir = EmitterCandidates(position, normal, kTracedEmitterCandidateCount, nearest2, random);
	bool visible;
	bool throughGlass;
	float3 irradiance = EmitterShade(reservoir, position, normal, push, nearest2, true, true, true, visible, throughGlass);
	return light + (throughGlass ? 0.0 : irradiance / kPi);
}

// the emitters' light a traced hit's voxel keeps (gGiKept: what arrives there and the way it arrives from, taken once at
// a static voxel's voxelizing, GiEmitterKept), as the indirect lighting's rays' hits take it (GiHitLitAt): its diffuse,
// what arrives at the hit's normal by the diffuse color over pi. the voxel the hit is in (a twentieth of a voxel into its surface)
// of the finest clip holding it, where it holds a surface facing the hit's way; none where it moves (its light isn't
// kept apart) or nothing facing so is voxelized there. the voxel that stands for the hit by GiHitVoxel's search of the
// voxels beside it cost Bistro at night 1.2 ms a frame at 1440p
float3 TracedEmitterKept(Surface surface, float3 view, float3 position, float3 normal, BvhHit hit)
{
	GiData gi = GiClip(0);
	if (gi.enabled == 0u)
		return 0.0;
	uint clipIt = 0;
	for (; clipIt + 1u < gi.clipCount; clipIt++)
	{
		GiClipView fade = GiClipViewAt(clipIt);
		if (all(abs(position - fade.fadeCenter.xyz) < fade.fadeExtent.xyz))
			break;
	}
	GiClipView clip = GiClipViewAt(clipIt);
	int3 at = int3(floor((position - clip.volumeOrigin.xyz) / clip.volumeOrigin.w - hit.normal * 0.05));
	uint3 size = clip.volumeSize.xyz;
	if (any(at < 0) || any(at >= int3(size)))
		return 0.0;
	uint index = clip.voxelFirst + GiVoxelIndex(uint3(at), size);
	GiVoxel voxel = gGiVoxels[index];
	if ((voxel.albedo >> 24) == 0u || dot(GiVoxelNormal(voxel.normal), hit.normal) < 0.5)
		return 0.0;
	GiVoxel still = gGiStaticVoxels[index];
	if (still.albedo != voxel.albedo || still.normal != voxel.normal)
		return 0.0;
	uint2 kept = gGiKept[index];
	return GiKeptInside(kept) ? 0.0 : surface.diffuseColor / 3.14159265 * GiKeptIrradiance(kept, normal);
}

// the near field rays a traced hit's light takes where its lookup is weak (see TracedIndirect): one in the visibility
// buffer's glossy reflections, which the Reflection passes filter over the surface and over frames, four in a fragment's
// sharp ones
static const uint kTracedNearRayCountGlossy = 1u;
static const uint kTracedNearRayCountSharp = 4u;

// the light near field rays from a traced hit gather (at position, normal toward the side the ray came from, off it by
// bias, in voxels voxel big; GiNearTrace), cosine distributed about the normal by the pixel and the frame (TracedRandom,
// offset from the glossy lobe's); fallback where none knows
float3 TracedNearLight(GiData gi, float3 position, float3 normal, float bias, float voxel, float3 fallback)
{
	uint count = TracedGlossyHere() ? kTracedNearRayCountGlossy : kTracedNearRayCountSharp;
	float2 first = TracedRandom(gFragmentPixel + uint2(17u, 59u));
	float3 t;
	float3 b;
	ShadowBasis(normal, t, b);
	float3 light = 0.0;
	float known = 0.0;
	for (uint rayIt = 0; rayIt < count; rayIt++)
	{
		float2 u = frac(first + float(rayIt) * float2(0.7548776662, 0.5698402910));
		float2 disk = sqrt(u.x) * float2(cos(6.28318530718 * u.y), sin(6.28318530718 * u.y));
		float3 direction = normalize(disk.x * t + disk.y * b + sqrt(max(1.0 - dot(disk, disk), 0.0)) * normal);
		float4 ray = GiNearTrace<false>(gi, position, normal, direction, bias, voxel);
		if (ray.a < 0.0)
			continue;
		light += ray.rgb;
		known += 1.0;
	}
	return known > 0.0 ? light / known : fallback;
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
// a transmissive surface gives its reflection of what is around it, as a hit's specular is taken (TracedIndirect: the
// environment as much as the probes see of it; not its own traced reflections), and the light through it (TracedPath, as
// the traced refractions take it), from inside its volume out of it (what is in the glass, a mosquito in amber,
// reflecting); valid as the path's. the opaque scene's structures hold no glass, and the rays passed through it: glass was
// missing from every reflection, CausticSpheres' amber sphere from its clear sphere's
float3 TracedRadiance(float3 origin, float3 direction, TracedCone cone, float roughness, out float t, out bool valid, out bool moving)
{
	GlassPath path;
	path.origin = origin;
	path.direction = direction;
	path.throughput = 1.0;
	path.inside = false;
	path.attenuationColor = 1.0;
	path.attenuationDistance = 0.0;
	return TracedPath(path, cone, 0.0, roughness, 1u + kTracedReflectionGlassSurfaceCount, true, t, valid, moving);
}

// the light along a path (see GlassPath) from where it is, through the transmissive surfaces (GlassPathStep), to what it
// meets of the opaque scene (TracedShade) or the environment (prefiltered for roughness), less what the surfaces keep, in
// at most queryCount queries. cone: the ray's where it started, traveled along the path to its origin. reflection: a
// reflection's ray (TracedRadiance), which may start inside a volume (where it meets its back), and takes the first
// surface's reflection; else the path's inside says. t: as TracedRadiance's, of its first query. valid: as TracedRadiance's,
// and false where its last query met glass still. moving: as TracedRadiance's, or the glass crossed moves.
//
// one loop the compiler doesn't unroll, with what shades a hit (TracedShade: the hit's lights and their shadow rays, the
// probes, the near field's rays) once after it: a reflection's first query and the path through the glass after it each
// had a copy, which made the kernels tracing them megabytes of Metal source on KosmicKrisp (see ComputeReflectionTrace)
float3 TracedPath(
	GlassPath path, TracedCone cone, float traveled, float roughness, uint queryCount, bool reflection, out float t, out bool valid,
	out bool moving)
{
	uint frameIndex = gPushConstants.frameIndex;
	float3 light = 0.0;
	t = kTracedMissDistance;
	valid = false;
	moving = false;
	bool reached = false; // whether the last query met the opaque scene or nothing (hit), which is shaded after the loop
	bool going = true;
	bool crossedGlass = false; // whether the path has met a transmissive surface (see TracedShade's fromGlass)
	BvhHit hit = BvhNothing(kTracedFar);
	gTracedHitEmission = 0.0;
	gTracedHitEmitter = false;
	[loop]
	for (uint queryIt = 0; queryIt < queryCount && going; queryIt++)
	{
		bool glass = false;
		if ((kSkippedLayers & SHADER_TYPES_LAYERS_GLASS) != 0u)
			hit = BvhTraceScene(path.origin, path.direction, kTracedFar, false, true, GiAlphaMask());
		else
			hit = BvhTraceSceneOrGlass(path.origin, path.direction, kTracedFar, GiAlphaMask(), glass);
		if (queryIt == 0u && BvhHitSomething(hit))
			t = hit.t;
		reached = !glass;
		going = glass;
		if (glass)
		{
			crossedGlass = true;
			moving = moving || hit.modelInstance == ~0u;
			GlassSurface surface = GlassSurfaceAt(path.origin, path.direction, hit, frameIndex);
			if (reflection && queryIt == 0u)
			{
				path.inside = TransmissionSolid(surface.material.transmission.y) && !hit.frontFace;
				// (inside, it absorbs as the volume it met the back of does, from where it started)
				path.attenuationColor = path.inside ? surface.material.attenuationColor.rgb : float3(1.0);
				path.attenuationDistance = path.inside ? surface.material.transmission.z : 0.0;
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
					TracedIndirect(surface.position, surface.normal, path.direction, glassRoughness, surface.normal, 0.0, indirect, specularRadiance);
					float nDotV = max(dot(surface.normal, -path.direction), 1e-4);
					light += EnvironmentBrdfApprox(float3(surface.f0), 1.0, glassRoughness, nDotV) * specularRadiance;
				}
			}
			traveled += hit.t;
			if (!GlassPathStep(path, surface, frameIndex, true))
			{
				valid = true;
				going = false;
			}
		}
	}
	if (reached)
	{
		cone.width += cone.spread * traveled;
		float shadedT;
		bool shadedMoving;
		float3 radiance =
			TracedShade(hit, path.origin, path.direction, cone, roughness, !reflection || crossedGlass, shadedT, valid, shadedMoving);
		moving = moving || shadedMoving;
		light += path.throughput * radiance;
		gTracedHitEmission *= path.throughput;
	}
	return light;
}

// what TracedRadiance gives of a ray's hit (or of nothing met). fromGlass: the ray leaves a transmissive surface (a
// refraction's, or a reflection's once it crossed glass), so a single sided triangle's back it meets is of something the
// glass is set into, whose inside isn't modelled, and is shaded as its front: ABeautifulGame's glass balls are sunk into
// their pawns and brass collars, modelled by their outsides alone. dropped as unknown, what the bottoms of the balls
// refract took the screen space lookup, which fades to the environment toward the view's edges; passed, as the rasterizer
// culls them, the pawns' tops showed through them in jagged holes (MosquitoInAmber's body, some of whose triangles face
// inward, comes out in lit facets instead). a ray leaving an opaque surface keeps the rule: it may have started inside
// something
float3 TracedShade(
	BvhHit hit, float3 origin, float3 direction, TracedCone cone, float roughness, bool fromGlass, out float t, out bool valid,
	out bool moving)
{
	valid = true;
	moving = false;
	if (!BvhHitSomething(hit))
	{
		t = kTracedMissDistance;
		// through the fog near the ground toward the sky along the ray (FogToSky), as the shading's environment is
		float3 fogLight;
		float3 fogTransmittance;
		FogToSky(gAerialPerspectiveData[gPushConstants.frameIndex][0], direction, origin.y, fogLight, fogTransmittance);
		return EnvironmentRadiance(gEnvironment[0], direction, roughness) * fogTransmittance + fogLight;
	}
	t = hit.t;
	moving = hit.modelInstance == ~0u;
	if (!fromGlass && !hit.frontFace && (hit.flags & BVH_TRIANGLE_FLAG_DOUBLE_SIDED) == 0u)
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
// a direction of its lobe, see TracedGlossyDirection, whose width widens the cone: spread). the ray starts off the
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

// a pixel's point of the unit square for the frame: Roberts' R2 sequence over the frames, offset per pixel by interleaved
// gradient noise (each coordinate its own), so that neighbors sample other directions and each pixel all of them in turn
float2 TracedRandom(uint2 pixel)
{
	return TracedRandom(pixel, TracedFrame());
}

// and its index-th point (several a frame: see TransmittedLobeRay)
float2 TracedRandom(uint2 pixel, uint index)
{
	float2 at = float2(pixel);
	float2 offset = float2(
		frac(52.9829189 * frac(dot(at, float2(0.06711056, 0.00583715)))),
		frac(52.9829189 * frac(dot(at + 17.0, float2(0.00583715, 0.06711056)))));
	return frac(offset + float(index & 0xffffu) * float2(0.7548776662, 0.5698402910));
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

// the visibility buffer's glossy reflection's direction at its pixel (see Shade): view reflected about a normal of the GGX
// lobe of the roughness (TracedVisibleNormal; the mirror direction where that falls below the surface), whose ray the
// Reflection Trace pass traces: one sample, which the Reflection passes filter over the surface and over frames, and the
// average of which is the lobe's (what the split sum's first term prefilters the environment by)
float3 TracedGlossyDirection(float3 geometryNormal, float3 normal, float3 view, float roughness, uint2 pixel)
{
	float alpha = max(roughness * roughness, 1e-3);
	float3 tangent = normalize(abs(normal.z) < 0.999 ? cross(float3(0.0, 0.0, 1.0), normal) : cross(float3(1.0, 0.0, 0.0), normal));
	float3 bitangent = cross(normal, tangent);
	float3 local = normalize(float3(dot(view, tangent), dot(view, bitangent), max(dot(view, normal), 1e-4)));
	float3 h = TracedVisibleNormal(local, alpha, TracedRandom(pixel));
	float3 direction = reflect(-view, h.x * tangent + h.y * bitangent + h.z * normal);
	if (dot(direction, normal) <= 0.0)
		direction = reflect(-view, normal);
	// as traced, for what is predicted along it (see Shade): along the direction below the geometry, the prediction saw
	// the ground where the ray skimming it saw the sky, and the edges of Bistro's lamp posts and scooter came out bright
	return TracedAboveGeometry(direction, geometryNormal);
}

// the density, per solid angle, of a glossy ray's direction (TracedGlossyDirection: view reflected about a normal of the
// GGX lobe of alpha about normal, by the normals visible from view) toward direction: the visible normals' density at the
// half vector over the reflection's Jacobian, G1(view) D(h) / (4 n.v); 0 below the surface
float TracedGlossyDensity(float3 normal, float3 view, float alpha, float3 direction)
{
	if (dot(normal, direction) <= 0.0)
		return 0.0;
	float nDotV = max(dot(normal, view), 1e-4);
	float nDotH = saturate(dot(normal, normalize(view + direction)));
	float alpha2 = alpha * alpha;
	float d = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
	float g1 = 2.0 * nDotV / (nDotV + sqrt(alpha2 + (1.0 - alpha2) * nDotV * nDotV));
	return g1 * alpha2 / (3.14159265 * d * d) / (4.0 * nDotV);
}

// the emitters' highlights in a glossy reflection (docs/research-scenes-plan.md, C1): a light sample beside its ray, an
// emitter point resampled (RIS) from kTracedGlossyEmitterCandidateCount candidates (EmitterPick's, a point of each) by
// what it gives the lobe (its emission's luminance by the lobe's density toward it, TracedGlossyDensity, and its area's
// solid angle), shadowed by a ray from off the surface (push, along geometryNormal), weighed against the ray meeting it by
// chance by the balance heuristic (the light's density EmitterDensity's, the lobe's its own: see ComputeReflectionTrace
// for the ray's share). its estimate of the lobe's average of the emitters' light, as the ray's radiance is of all of it.
// a point nearer than nearest (squared) is taken as that far, as the emitters' samples take it. take: what the pixel
// takes of the estimate (its weight over the ray's prediction, see ComputeReflectionTrace), least: kTracedGlossyEmitterTracedLeast
// of white at the view's exposure, below which the shadow ray is a gamble (by the estimate unshadowed, as the pixel takes it)
float3 TracedGlossyEmitters(
	float3 position, float3 geometryNormal, float3 normal, float3 view, float alpha, float push, float nearest2, float3 take, float least, uint2 pixel)
{
	EmitterRandom random = EmitterRandomAt(pixel ^ uint2(0x7f4au, 0x1b87u), TracedFrame());
	EmitterReservoir reservoir = EmitterEmpty();
	// the chosen candidate's point, the lobe's density toward it, and what it gives the lobe unshadowed (by its emission)
	EmitterPoint chosen;
	chosen.position = 0.0;
	chosen.normal = 0.0;
	chosen.emission = 0.0;
	chosen.area = 0.0;
	chosen.doubleSided = false;
	float chosenLobe = 0.0;
	float chosenGives = 0.0;
	for (uint candidateIt = 0; candidateIt < kTracedGlossyEmitterCandidateCount; candidateIt++)
	{
		float probability;
		uint triangle = EmitterPick(random, position, normal, probability);
		float2 barycentrics = EmitterBarycentrics(random);
		if (triangle == ~0u)
			continue;
		EmitterPoint point = EmitterPointAt(triangle, barycentrics);
		float3 offset = point.position - position;
		float distance2 = dot(offset, offset);
		float3 toPoint = offset * rsqrt(max(distance2, 1e-30));
		float emitted = -dot(point.normal, toPoint);
		if (point.doubleSided)
			emitted = abs(emitted);
		float lobe = dot(geometryNormal, toPoint) > 0.0 ? TracedGlossyDensity(normal, view, alpha, toPoint) : 0.0;
		float gives = lobe * max(emitted, 0.0) / max(distance2, nearest2);
		float target = EmitterLuminance(point.emission) * gives;
		// its density per area: the triangle's probability, uniformly over it
		if (reservoir.Add(triangle, barycentrics, target, target * point.area / max(probability, 1e-30), random))
		{
			chosen = point;
			chosenLobe = lobe;
			chosenGives = gives;
		}
	}
	reservoir.count = float(kTracedGlossyEmitterCandidateCount);
	reservoir.Finish();
	if (reservoir.triangle == ~0u)
		return 0.0;
	float light = EmitterDensity(position, normal, chosen.position);
	float3 estimate = chosen.emission * chosenGives * reservoir.weight * (light / (light + chosenLobe));
	// Russian roulette on the shadow ray, by what the pixel would take of the estimate
	float takes = EmitterLuminance(take * estimate);
	if (takes < least)
	{
		float chance = takes / least;
		if (!(random.Next() < chance))
			return 0.0;
		estimate /= chance;
	}
	float3 origin = position + geometryNormal * push;
	float3 ray = chosen.position - origin;
	float span = length(ray);
	if (ShadowOccluder(origin, ray / max(span, 1e-30), span * 0.999, gEmitterData[gPushConstants.frameIndex][0].movingBvh != 0u) >= 0.0)
		return 0.0;
	return estimate;
}

// a fragment's sharp rays from its point at position (see kTracedFragmentBase), those traced says (none in compute):
// light their light (rgb, and in a 1 where what it met is known, see TracedRadiance's valid; 0 where none is traced).
// the reflections leave the surface as TracedReflection's ray does. the refractions from the surface along their
// directions (into its volume, refracted: volume; through a thin wall, as the view goes), through the transmissive
// surfaces as the caustics' photons go (TracedPath, GlassPathStep: out of the volume where it meets its back, refracted,
// or reflected back in where it can't leave; into or through the glass beyond), to what they meet of the opaque scene,
// less what the surfaces past this one keep (this one's own attenuation and Fresnel are TransmittedLight's). the opaque
// scene's structures hold no glass, and the ray used to pass through its own volume without leaving it: a ball showed what
// is behind it shifted rather than turned upside down, CausticSpheres' clear sphere the ground beyond its shadow, lit. the
// glass that moves is in a structure of its own, refit each frame (see AccelerationStructures). from the surface, not from
// where the screen space lookup has the ray leave the volume (at the material's thickness along the ray, see
// TransmittedLight): USDShaderBallForGltf's glass shell has a thickness of up to 8.9 in a model a tenth of a unit
// across, which put the exit inside the core (its back met: black) or past everything (the environment's), a wavy border
// between the two.
//
// all through one loop the compiler doesn't unroll, so that what shades a hit (TracedPath) is in the fragment shader
// once: traced where each was taken, every ray (the base's and the clearcoat's reflections, the refraction, a refraction
// of each color in dispersion's loop) had a copy, and the main pass's fragment shader was 22.5 MB of Metal source on
// KosmicKrisp, its pipeline half a minute to make cold
void TracedFragmentRays(
	float3 position, float3 geometryNormal, float3 directions[kTracedFragmentRayCount], bool traced[kTracedFragmentRayCount],
	bool volume, float3 attenuationColor, float attenuationDistance, out float4 light[kTracedFragmentRayCount])
{
	TracedCone cone = TracedPixelCone(position);
	float3 eye = gViewData[gPushConstants.frameIndex][gShadingViewId].eyePosition.xyz;
	float rounding = 1e-4 * distance(eye, position);
	[loop]
	for (uint rayIt = 0; rayIt < kTracedFragmentRayCount; rayIt++)
	{
		light[rayIt] = 0.0;
		if (traced[rayIt])
		{
			bool refraction = rayIt >= kTracedFragmentRefraction;
			GlassPath path;
			path.direction = refraction ? directions[rayIt] : TracedAboveGeometry(directions[rayIt], geometryNormal);
			float push = max((refraction ? 1.0 : 1.5) * cone.width, rounding);
			path.origin = position + (refraction ? path.direction : geometryNormal) * push;
			path.throughput = 1.0;
			path.inside = refraction && volume;
			// (a refraction into the volume absorbs as the surface's material has it, along the path to where it leaves:
			// see GlassPathStep's alongPath; TransmittedLight leaves the thickness's absorption out of it)
			path.attenuationColor = path.inside ? attenuationColor : float3(1.0);
			path.attenuationDistance = path.inside ? attenuationDistance : 0.0;
			float t;
			bool valid;
			bool moving;
			float3 radiance = TracedPath(
				path, cone, refraction ? push : 0.0, 0.0,
				refraction ? SHADER_TYPES_CAUSTIC_SURFACE_COUNT : 1u + kTracedReflectionGlassSurfaceCount, !refraction, t, valid, moving);
			// through the air between the surface and what the ray met, as if that lay along the eye's ray that much
			// farther (AerialPerspectiveBetween); a miss has the fog toward the sky (TracedShade)
			if (valid && t < kTracedMissDistance)
			{
				float3 airLight;
				float3 airTransmittance;
				float near = distance(eye, position);
				AerialPerspectiveBetween(near, near + t, airLight, airTransmittance);
				radiance = radiance * airTransmittance + airLight;
			}
			light[rayIt] = float4(radiance, valid ? 1.0 : 0.0);
		}
	}
}
#endif

// the visibility buffer's reflections, traced (the Reflection Trace pass, after Main Visibility, whose shading records
// their rays: see TracedReflectionRecord, TracedCoatRecord), from each pixel's point (its depth) off its geometry (the
// pre-pass's normal): the glossy ray's radiance as a share of what was predicted along it (the prediction itself where
// what it met isn't known), and its hit distance, which the Reflection passes filter; and the clearcoat's sharp
// reflection's difference from the environment prefiltered along it, by its weight, added to the main color. both rays go
// through one call in a loop the compiler doesn't unroll, so that what shades a hit (TracedPath: the glass, the hit's
// lights and their shadow rays, the probes, the near field's rays) is in the kernel once: traced in the shading itself, a
// copy for each ray made the visibility buffer's generic kernel 31.5 MB of Metal source on KosmicKrisp, which took two
// minutes to compile
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeReflectionTrace(uint3 dispatchThreadId: SV_DispatchThreadID)
{
#if BVH_RAY_QUERY
	uint2 extent;
	gRWTextures[SHADER_TYPES_REFLECTION_RAW_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(pixel >= int2(extent)))
		return;
	float4 weight = gTextures[SHADER_TYPES_REFLECTION_WEIGHT_TEXTURE].Load(int3(pixel, 0));
	bool glossy = weight.a >= 0.0;
	float4 coat = gTextures[SHADER_TYPES_REFLECTION_COAT_TEXTURE].Load(int3(pixel, 0));
	ViewData view;
	uint viewId;
	ShadowSurface surface;
	if ((!glossy && coat.w <= 0.0) || !ViewAt(pixel, view, viewId) || !ShadowSurfaceAt(pixel, view, surface))
		return;
	gShadingViewId = viewId;
	gFragmentPixel = uint2(pixel);
	float4 ray = gTextures[SHADER_TYPES_REFLECTION_RAY_TEXTURE].Load(int3(pixel, 0));
	uint rayEnd = coat.w > 0.0 ? 2u : 1u;
	[loop]
	for (uint rayIt = glossy ? 0u : 1u; rayIt < rayEnd; rayIt++)
	{
		bool isCoat = rayIt == 1u;
		float3 direction = GiFromOctahedral(isCoat ? coat.xy : ray.xy);
		// the glossy ray's cone widens by its lobe (as its alpha); the coat's is a mirror's, as a fragment's sharp one
		float roughness = isCoat ? 0.0 : ReflectionRoughness(weight.a);
		float spread = isCoat ? 0.0 : max(roughness * roughness, 1e-3);
		float t;
		bool valid;
		bool moving;
		float3 radiance = TracedReflection(surface.position, surface.normal, direction, spread, roughness, t, valid, moving);
		float3 emitted = gTracedHitEmitter && !isCoat ? gTracedHitEmission : float3(0.0);
		// through the air between the surface and what the ray met, as if that lay along the eye's ray that much farther
		// (AerialPerspectiveBetween); a miss has the fog toward the sky (TracedShade)
		if (valid && t < kTracedMissDistance)
		{
			float3 airLight;
			float3 airTransmittance;
			float near = distance(view.eyePosition.xyz, surface.position);
			AerialPerspectiveBetween(near, near + t, airLight, airTransmittance);
			radiance = radiance * airTransmittance + airLight;
			emitted *= airTransmittance;
		}
		if (isCoat)
		{
			if (valid)
				gMainColor[pixel] += float4(coat.w * (radiance - EnvironmentRadiance(gEnvironment[0], direction, coat.z)), 0.0);
		}
		else
		{
			float3 expected = gRWTextures[SHADER_TYPES_REFLECTION_RAW_RW_TEXTURE][pixel].rgb;
			float3 predicted = expected + kReflectionPredictionLeast / max(view.exposure, 1e-6);
			// where what the ray met isn't known (see TracedRadiance), what they predict, unblurred. a sample whose light
			// the pixel takes (by the weight, as ComputeReflectionTemporal adds it) is far brighter than white at the view's
			// exposure is taken as a few times that: one that met what is very bright (a lamp's bulb, the sun's glint off a
			// mirror), which the filter spread into a square, alone among its neighbors'; tonemapped, they look the same. by
			// the light the pixel takes, not the sample's own: a lobe's average of what it met, a bright window over a part
			// of it, is what a faint specular takes a share of, and bathroom2's wooden floor (a coat reflecting some 5% of
			// it) lost most of the emissive window's glossy reflection, whose samples were taken as 8 times white
			float4 sample = float4(expected, 0.0);
			if (valid)
			{
				// the emitters' highlights, where emissive surfaces are lights: what the ray met of them by the balance
				// heuristic's share (the lobe's density toward it against the light samples', EmitterDensity), and a light
				// sample (TracedGlossyEmitters) the rest; neither capped below, a small bright light's highlight being
				// theirs (the cap had the smooth plate's image of veach-mis's smallest sphere dark in its center)
				float3 highlights = 0.0;
				if (EmittersLight())
				{
					radiance -= emitted;
					float3 glossyNormal = GiFromOctahedral(ray.zw);
					float3 toEye = normalize(view.eyePosition.xyz - surface.position);
					float alpha = max(roughness * roughness, 1e-3);
					if (any(emitted > 0.0))
					{
						float lobe = TracedGlossyDensity(glossyNormal, toEye, alpha, direction);
						float light = EmitterDensity(surface.position, glossyNormal, gTracedHitPoint);
						highlights = lobe + light > 0.0 ? emitted * (lobe / (lobe + light)) : emitted;
					}
					TracedCone cone = TracedPixelCone(surface.position);
					float push = max(1.5 * cone.width, 1e-4 * distance(view.eyePosition.xyz, surface.position));
					highlights += TracedGlossyEmitters(
						surface.position, surface.normal, glossyNormal, toEye, alpha, push, push * push, weight.rgb / predicted,
						kTracedGlossyEmitterTracedLeast / max(view.exposure, 1e-6), uint2(pixel));
				}
				float luminance = dot(weight.rgb * radiance / predicted, float3(0.2126, 0.7152, 0.0722));
				float most = kReflectionMostWhite / max(view.exposure, 1e-6);
				if (luminance > most)
					radiance *= most / luminance;
				sample = float4(radiance + highlights, ReflectionPackDistance(min(t, kTracedMissDistance), moving));
			}
			sample.rgb /= predicted;
			gRWTextures[SHADER_TYPES_REFLECTION_RAW_RW_TEXTURE][pixel] = sample;
		}
	}
#endif
}

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
			// 0 where there is no image to take: ReflectionVirtualHistory writes them only where there is
			float imageWeights = 0.0;
			float imageHeld = 0.0;
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
