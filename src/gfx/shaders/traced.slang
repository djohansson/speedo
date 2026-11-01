// the traced reflections and refractions (PushConstants::traced, TRACED_*; View > Ray Traced Reflections and
// Refractions): where the device has ray queries (BVH_RAY_QUERY), the radiance along a smooth surface's reflection
// (TracedReflection, see Shade) and the light through a transmissive one (TracedRefraction, see TransmittedLight), traced
// through the model's acceleration structures (gBvhScene[frame], BvhTraceScene) and shaded where the ray meets something
// (TracedHitRadiance), the environment where it meets nothing. sharp: a ray along the reflection's dominant direction
// (or the refracted ray), all of the reflection below kTracedRoughnessFull, faded into the glossy one the probes and the
// environment give (or the transmission texture's mips) by kTracedRoughnessEnd. a part of shaders.slang, which includes
// it after capi.h and sky.h

// the roughness up to which a reflection (or a transmission's blur, see TransmittedLight) is all traced, and from which
// none of it is: a ray along the dominant direction is a mirror's reflection, too sharp for a glossy surface
static const float kTracedRoughnessFull = 0.15;
static const float kTracedRoughnessEnd = 0.4;
// how far the rays go, in the scene's units (any scene's: what they meet is nearer)
static const float kTracedFar = 1.0e30;
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

bool TracedReflectionsOn()
{
	return (gPushConstants.traced & TRACED_REFLECTIONS) != 0u;
}

bool TracedRefractionsOn()
{
	return (gPushConstants.traced & TRACED_REFRACTIONS) != 0u;
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
// light where they are lights (see EmitterIrradiance, a screen's worth of traced pixels). a back face of a single sided
// triangle (seen from inside something, or a plane seen from behind) is shaded as if double sided. coneWidth: the ray's
// cone's width at the hit
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
	// distance (the hit's rounding)
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
		radiance += BaseLobe(surface, view, toLight, nDotL) * nDotL * light.intensity.rgb * falloff;
	}

	// the indirect lighting's, as Shade takes it: the probes' diffuse where they cover the hit (none where a clip holds it
	// but no probe sees it), else the environment's; and the environment's specular as much as the probes see of it,
	// their light along the reflection as it gets rough
	EnvironmentData environment = gEnvironment[0];
	float3 reflected = DominantDirection(normal, reflect(direction, normal), surface.roughness);
	float3 specularRadiance = EnvironmentRadiance(environment, reflected, surface.roughness);
	GiSample sample = GiSampleSurface(position, normal, view);
	float3 indirect = sample.valid ? GiEvaluate(sample, normal) : (sample.inside ? 0.0 : -1.0);
	if (indirect.x >= 0.0)
	{
		float3 luminance = float3(0.2126, 0.7152, 0.0722);
		float seen = saturate(dot(indirect, luminance) / max(dot(EnvironmentIrradiance(environment, normal), luminance), 1e-4));
		specularRadiance = lerp(specularRadiance * seen, GiEvaluate(sample, reflected), smoothstep(0.2, 0.9, surface.roughness));
	}
	else
		indirect = EnvironmentIrradiance(environment, normal);
	radiance += surface.diffuseColor * indirect +
				EnvironmentBrdfApprox(surface.f0, surface.f90, surface.roughness, nDotV) * specularRadiance;
	return radiance;
}

// the radiance along a ray from origin: what it meets of the acceleration structures (what moves too, alpha masks cut
// out), shaded, else the environment along it (the sky's sun included). cone: the ray's at origin
float3 TracedRadiance(float3 origin, float3 direction, TracedCone cone)
{
	BvhHit hit = BvhTraceScene(origin, direction, kTracedFar, false, true, GiAlphaMask());
	if (!BvhHitSomething(hit))
		return EnvironmentRadiance(gEnvironment[0], direction, 0.0);
	return TracedHitRadiance(direction, hit, origin + direction * hit.t, cone.width + cone.spread * hit.t);
}

// the radiance a surface at position reflects along direction from (its reflection's dominant direction, see Shade). the
// ray starts off the surface on the side it goes to (the geometry's, facing the viewer), by a pixel and a half and a ten
// thousandth of the distance from the eye (the depth's and the structures' rounding), as the traced shadows' rays do;
// a normal map that turns it below the geometry has it skim the surface instead, which it would otherwise meet at once
float3 TracedReflection(float3 position, float3 geometryNormal, float3 direction)
{
	TracedCone cone = TracedPixelCone(position);
	float below = dot(direction, geometryNormal) - 0.02;
	if (below < 0.0)
		direction = normalize(direction - geometryNormal * below);
	float3 eye = gViewData[gPushConstants.frameIndex][gShadingViewId].eyePosition.xyz;
	float push = max(1.5 * cone.width, 1e-4 * distance(eye, position));
	return TracedRadiance(position + geometryNormal * push, direction, cone);
}

// the light through a transmissive surface at position, along the refracted ray from where it leaves the volume (exit,
// see TransmittedLight: the transmissive surfaces aren't in the acceleration structures, see bvh::Triangles, so it
// meets neither its own back nor other glass)
float3 TracedRefraction(float3 position, float3 exit, float3 direction)
{
	TracedCone cone = TracedPixelCone(position);
	float travelled = distance(position, exit);
	cone.width += cone.spread * travelled;
	float3 eye = gViewData[gPushConstants.frameIndex][gShadingViewId].eyePosition.xyz;
	float push = max(cone.width, 1e-4 * distance(eye, position));
	return TracedRadiance(exit + direction * push, direction, cone);
}
#endif
