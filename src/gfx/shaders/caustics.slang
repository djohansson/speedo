// caustics: the light the transmissive surfaces refract, as photons from the lights' shadow views (ComputeCaustics) into
// their tiles of the shadow transmittance (ComputeCausticsResolve), which the points behind the surfaces take in place of
// what goes straight through them (see ShadowTransmittance). a part of shaders.slang, which includes it after capi.h and
// sky.h

// a point in the unit square for a photon (pcg2d, Jarzynski and Olano)
float2 CausticJitter(uint2 photon)
{
	uint2 v = photon * 1664525u + 1013904223u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	return float2(v >> 8u) / 16777216.0;
}

// whether a point is in a shadow view's tile (and between its near and far planes), as its texels hold it
bool CausticViewHolds(ShadowData view, float3 position)
{
	float4 clip = mul(view.viewProjection, float4(position, 1.0));
	if (clip.w <= 0.0)
		return false;
	float3 ndc = clip.xyz / clip.w;
	float2 local = ndc.xy * 0.5 + 0.5;
	return all(local >= 0.0) && all(local < 1.0) && ndc.z >= 0.0 && ndc.z <= 1.0;
}

// where a photon lands on an opaque surface: its light added to one of its light's views' transmittance texels around the
// surface's place in the view (bilinear, in fixed point), if the surface is what the view sees nearest there (what the
// lookup lights: a surface behind another, from the light, can't be told apart from it in the view)
void CausticSplatView(ShadowData view, float3 position, float3 normal, float3 power, float2 size)
{
	float4 clip = mul(view.viewProjection, float4(position, 1.0));
	if (clip.w <= 0.0)
		return;
	float3 ndc = clip.xyz / clip.w;
	float2 local = ndc.xy * 0.5 + 0.5;
	if (any(local < 0.0) || any(local > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
		return;
	ShadowMapPlace place;
	place.view = view;
	place.uv = view.atlasRect.xy + local * view.atlasRect.zw;
	place.depth = ndc.z;
	Texture2D atlas = gTextures[SHADER_TYPES_SHADOW_ATLAS_TEXTURE];
	float2 atlasSize;
	atlas.GetDimensions(atlasSize.x, atlasSize.y);
	float texelSize = view.params.x * (view.params.y > 0.0 ? ShadowViewDistance(view, ndc.z) : 1.0);
	float2 gradient = ShadowReceiverGradient(place, position, normal, texelSize);
	float slack = abs(dot(abs(gradient), 1.0 / atlasSize)) * 3.0 + 1e-5;
	float stored = atlas.Load(int3(int2(place.uv * atlasSize), 0)).r;
	if (stored > place.depth + slack) // depth reversed: something nearer the light is there
		return;

	float2 low = view.atlasRect.xy * size;
	float2 high = (view.atlasRect.xy + view.atlasRect.zw) * size - 1.0;
	float2 st = place.uv * size - 0.5;
	float2 base = floor(st);
	float2 f = st - base;
	float weights[4] = {(1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y};
	for (uint cornerIt = 0; cornerIt < 4; cornerIt++)
	{
		float2 corner = base + float2(cornerIt & 1u, cornerIt >> 1u);
		if (any(corner < low) || any(corner > high))
			continue;
		uint3 amount = uint3(power * weights[cornerIt] * SHADER_TYPES_CAUSTIC_FIXED_POINT_SCALE + 0.5);
		uint index = (uint(corner.y) * uint(size.x) + uint(corner.x)) * 3u;
		for (uint channel = 0; channel < 3; channel++)
			if (amount[channel] > 0u)
				InterlockedAdd(gCausticPhotons[index + channel], amount[channel]);
	}
}

// where a photon of a shadow view lands: in each of its light's views whose tile holds the place (a lens throws its light
// sideways, into the cascades or cube faces beside the one the glass is in), its share of the view's texel by their sizes
void CausticSplat(uint frameIndex, ShadowData source, float3 position, float3 normal, float3 power, float2 size)
{
	for (uint viewIt = source.lightViews.x; viewIt < source.lightViews.x + source.lightViews.y; viewIt++)
	{
		ShadowData view = gShadowViews[frameIndex][viewIt];
		float scale = source.params.x / view.params.x;
		CausticSplatView(view, position, normal, power * (scale * scale), size);
	}
}

#if BVH_RAY_QUERY
// a ray's way through the transmissive surfaces that don't move (BvhTraceGlass, the ray queries' structure of them): the
// caustics' photons' (ComputeCaustics) and the traced refractions' and reflections' (TracedGlassPath). where it is and
// goes, what it carries of its light, and whether it is inside a volume
struct GlassPath
{
	float3 origin;
	float3 direction;
	float3 throughput;
	bool inside;
};

// a transmissive surface a ray met (glass, from origin along direction): where, its normal as interpolated (the
// triangles' facets drew the caustics as polygons) on the side the ray comes from (the geometry's where they disagree),
// and the instance that places it (see BvhHit::normalInstance), its material and what it has at the point, its ior and
// its dielectric's reflectance at normal incidence, and how far off it a ray goes on (its rounding)
struct GlassSurface
{
	float3 position;
	float3 normal;
	uint normalInstance;
	MaterialData material;
	float4 texCoord01;
	float3 vertexColor;
	float ior;
	float f0;
	float eps;
};

GlassSurface GlassSurfaceAt(float3 origin, float3 direction, BvhHit glass, uint frameIndex)
{
	GlassSurface surface;
	surface.position = origin + direction * glass.t;
	surface.material = gMaterialData[glass.flags >> BVH_TRIANGLE_MATERIAL_SHIFT];
	float3 barycentrics = float3(1.0 - glass.barycentrics.x - glass.barycentrics.y, glass.barycentrics);
	surface.texCoord01 = 0.0;
	float3 vertexNormal = 0.0;
	surface.vertexColor = 0.0;
	for (uint cornerIt = 0; cornerIt < 3; cornerIt++)
	{
		VertexP3fN3fTa4fT014fC4f corner = gVertexBuffer[gIndexBuffer[glass.firstIndex + cornerIt]];
		surface.texCoord01 += corner.texCoord01 * barycentrics[cornerIt];
		vertexNormal += corner.normal * barycentrics[cornerIt];
		surface.vertexColor += corner.color.rgb * barycentrics[cornerIt];
	}
	// (glass that moves rigidly, following a node: by its instance as the frame placed it; skinned or morphed: the
	// triangle's own)
	float3 normal = glass.normal;
	if (glass.normalInstance != ~0u)
	{
		normal = normalize(mul(gModelInstances[frameIndex][glass.normalInstance].inverseTransposeModelTransform, float4(vertexNormal, 0.0)).xyz);
		normal = dot(normal, glass.normal) > 0.0 ? normal : (dot(normal, glass.normal) < 0.0 ? -normal : glass.normal);
	}
	surface.normal = all(isfinite(normal)) ? normal : glass.normal;
	surface.normalInstance = glass.normalInstance;
	surface.ior = max(surface.material.specularColor.a, 1.0);
	surface.f0 = (surface.ior - 1.0) / (surface.ior + 1.0);
	surface.f0 *= surface.f0;
	float3 position = surface.position;
	surface.eps = max(1e-4, 1e-5 * max(abs(position.x), max(abs(position.y), abs(position.z))));
	return surface;
}

// a path's step at the transmissive surface it met (glass): into a surface, what it lets through (its transmission of the
// dielectric, the base color at the point, less its Fresnel reflection, by its ior), refracted about the surface's normal,
// into a volume (a thickness factor; a thin wall lets it through as it was going), attenuated over its thickness
// (KHR_materials_volume's attenuation, as the view through it has it); out of a volume, less its Fresnel reflection, or
// reflected back in where it can't leave. false where it carries nothing on
bool GlassPathStep(inout GlassPath path, BvhHit glass, uint frameIndex)
{
	GlassSurface surface = GlassSurfaceAt(path.origin, path.direction, glass, frameIndex);
	float3 position = surface.position;
	float3 normal = surface.normal;
	MaterialData material = surface.material;
	float4 texCoord01 = surface.texCoord01;
	float3 vertexColor = surface.vertexColor;
	float ior = surface.ior;
	float f0 = surface.f0;
	float eps = surface.eps;

	if (path.inside)
	{
		// out of the volume: refracted out about the normal facing back inside, else reflected back in
		float3 refracted = refract(path.direction, normal, ior);
		if (dot(refracted, refracted) == 0.0)
		{
			path.direction = reflect(path.direction, normal);
			path.origin = position + normal * eps;
			return true;
		}
		refracted = normalize(refracted);
		path.throughput *= 1.0 - FresnelSchlick(float3(f0), 1.0, saturate(dot(refracted, -normal))).r;
		path.direction = refracted;
		path.origin = position - normal * eps;
		path.inside = false;
		return true;
	}

	// into a surface: what it lets through, at the point
	float3 color = material.color.rgb * vertexColor;
	if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
	{
		TextureView textureView = gTextureViews[material.baseColorView];
		color *= SampleViewLevel(textureView, ViewTexCoord(textureView, texCoord01), 0.0).rgb;
	}
	float metallic = material.metallic;
	if ((material.flags & MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE) != 0u)
	{
		TextureView textureView = gTextureViews[material.metallicRoughnessView];
		metallic *= SampleViewLevel(textureView, ViewTexCoord(textureView, texCoord01), 0.0).g;
	}
	float transmission = (material.flags & MATERIAL_FLAG_TRANSMISSION) != 0u ? material.transmission.x : 0.0;
	if ((material.flags & MATERIAL_FLAG_TRANSMISSION_TEXTURE) != 0u)
	{
		TextureView textureView = gTextureViews[material.transmissionView];
		transmission *= SampleViewLevel(textureView, ViewTexCoord(textureView, texCoord01), 0.0).r;
	}
	float fresnel = FresnelSchlick(float3(f0), 1.0, saturate(dot(-path.direction, normal))).r;
	path.throughput *= transmission * (1.0 - metallic) * color * (1.0 - fresnel);
	if (all(path.throughput <= 0.0))
		return false;
	// a thin wall: through both its faces as it was going
	if (material.transmission.y <= 0.0)
	{
		path.throughput *= 1.0 - fresnel;
		path.origin = position + path.direction * eps;
		return true;
	}
	// a volume: attenuated over its thickness where it enters (its factor, its texture's, the instance's scale), as the
	// view through it takes it (TransmittedLight) and the transmittance's tint did: along the path inside,
	// DragonAttenuation's dragon (an attenuation distance of 0.155 m in a body some 0.6 m across, where its thickness is up
	// to 0.57) passed almost none, an orange dragon casting a black shadow
	float thickness = material.transmission.y;
	if ((material.flags & MATERIAL_FLAG_THICKNESS_TEXTURE) != 0u)
	{
		TextureView textureView = gTextureViews[material.thicknessView];
		thickness *= SampleViewLevel(textureView, ViewTexCoord(textureView, texCoord01), 0.0).r;
	}
	if (surface.normalInstance != ~0u) // a skinned or morphed glass's at its mesh's scale
	{
		float4x4 transform = gModelInstances[frameIndex][surface.normalInstance].modelTransform;
		thickness *= pow(abs(determinant(float3x3(transform[0].xyz, transform[1].xyz, transform[2].xyz))), 1.0 / 3.0);
	}
	path.throughput *= VolumeAttenuation(material.attenuationColor.rgb, material.transmission.z, thickness);
	path.direction = normalize(refract(path.direction, normal, 1.0 / ior));
	path.origin = position - normal * eps;
	path.inside = true;
	return true;
}
#endif

// the caustics' photons of the shadow view PushConstants::shadowView, of its tile of the shadow transmittance: from each of
// SHADER_TYPES_CAUSTIC_PHOTONS_PER_AXIS squared points of each texel where a transmissive surface is (its alpha the nearest
// one's depth), along the light, through the transmissive surfaces that don't move (GlassPathStep at each), until it meets
// an opaque surface (CausticSplat), at most SHADER_TYPES_CAUSTIC_SURFACE_COUNT surfaces. a photon meeting an
// opaque surface before the transmissive one its texel has is the light straight to it, which the lookup gives it, and is
// dropped; one meeting none (a point of the texel past the surface's edge) lands where the light straight on goes, which
// the lookup takes away there (the texel is behind the surface). a photon's light is its share of its texel's, in units of
// what the light gives a surface unshadowed. each point of the glass is traced once of its light's views, by the finest
// whose tile holds it (they are finest first, see shadows::Plan), and lands in all of them (CausticSplat): the renderer
// traces every view of a light one of which is drawn again
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeCaustics(uint3 dispatchThreadId : SV_DispatchThreadID)
{
#if BVH_RAY_QUERY
	uint frameIndex = gPushConstants.frameIndex;
	ShadowData view = gShadowViews[frameIndex][gPushConstants.shadowView];
	uint2 imageSize;
	gShadowTransmittanceImage.GetDimensions(imageSize.x, imageSize.y);
	float2 size = float2(imageSize);
	float2 tileOrigin = view.atlasRect.xy * size;
	float2 tileExtent = view.atlasRect.zw * size;
	uint photons = SHADER_TYPES_CAUSTIC_PHOTONS_PER_AXIS;
	uint2 texel = dispatchThreadId.xy / photons;
	if (any(float2(texel) >= tileExtent))
		return;
	float nearest = gShadowTransmittanceImage[int2(tileOrigin) + int2(texel)].a;
	if (nearest <= 0.0)
		return;

	// each photon at a point of its part of the texel, by its place in the transmittance (the same every frame): at their
	// parts' centers, a regular grid, a sphere's refraction drew it as rings of moire
	float2 local = (float2(texel) + (float2(dispatchThreadId.xy % photons) + CausticJitter(uint2(tileOrigin) * photons + dispatchThreadId.xy)) / float(photons)) / tileExtent;
	float2 ndc = local * 2.0 - 1.0;
	// a finer view of the light holding the glass there traces it (at the texel's nearest layer)
	float4 glassPoint = mul(view.inverseViewProjection, float4(ndc, nearest, 1.0));
	for (uint finerIt = view.lightViews.x; finerIt < gPushConstants.shadowView; finerIt++)
		if (CausticViewHolds(gShadowViews[frameIndex][finerIt], glassPoint.xyz / glassPoint.w))
			return;
	float4 nearPoint = mul(view.inverseViewProjection, float4(ndc, 1.0, 1.0)); // depth reversed: 1 at the near plane
	float4 farPoint = mul(view.inverseViewProjection, float4(ndc, 0.0, 1.0));
	GlassPath path;
	path.origin = nearPoint.xyz / nearPoint.w;
	path.direction = normalize(farPoint.xyz / farPoint.w - path.origin);
	path.throughput = 1.0 / float(photons * photons);
	path.inside = false;
	bool passed = false; // through a transmissive surface
	for (uint surfaceIt = 0; surfaceIt < SHADER_TYPES_CAUSTIC_SURFACE_COUNT; surfaceIt++)
	{
		bool glass;
		BvhHit hit = BvhTraceSceneOrGlass(path.origin, path.direction, 1e30, GiAlphaMask(), glass);
		if (!BvhHitSomething(hit))
			return; // left the scene (or out of an open volume)
		if (!glass)
		{
			// lands, but the light straight to a surface in front of the transmissive one (glass beyond it); inside a volume
			// too (a core in a shell, a mosquito in amber, the ground behind glass modelled as one surface of a thickness,
			// which no photon leaves): dropped there, what is inside the glass took no light, which the traced refractions
			// show through it
			if (passed || !BvhHitSomething(BvhTraceGlass(path.origin, path.direction, 1e30)))
				CausticSplat(frameIndex, view, path.origin + path.direction * hit.t, hit.normal, path.throughput, size);
			return;
		}
		passed = passed || !path.inside;
		if (!GlassPathStep(path, hit, frameIndex))
			return;
	}
#endif
}

// a shadow view's caustics, from its photons (gCausticPhotons) into its tile of the shadow transmittance's rgb (its alpha,
// the nearest transmissive surface's depth, kept)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeCausticsResolve(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	ShadowData view = gShadowViews[gPushConstants.frameIndex][gPushConstants.shadowView];
	uint2 imageSize;
	gShadowTransmittanceImage.GetDimensions(imageSize.x, imageSize.y);
	float2 tileOrigin = view.atlasRect.xy * float2(imageSize);
	float2 tileExtent = view.atlasRect.zw * float2(imageSize);
	if (any(float2(dispatchThreadId.xy) >= tileExtent))
		return;
	int2 at = int2(tileOrigin) + int2(dispatchThreadId.xy);
	uint index = (uint(at.y) * imageSize.x + uint(at.x)) * 3u;
	float3 light = float3(gCausticPhotons[index], gCausticPhotons[index + 1u], gCausticPhotons[index + 2u]) / SHADER_TYPES_CAUSTIC_FIXED_POINT_SCALE;
	gShadowTransmittanceImage[at] = float4(light, gShadowTransmittanceImage[at].a);
}
