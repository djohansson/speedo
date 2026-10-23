// the punctual lights' shadows from the atlas, or the traced ones (see tracedshadows.slang): ShadowFactor. a part of
// shaders.slang, which includes it after capi.h and sky.h

// the fragment being shaded's pixel, and whether it takes the traced shadows and the indirect lighting's near field there
// (the main pass's opaque fragments, at the depth the pre-pass drew: PushConstants::prepassImages), set by the fragment
// entry points
static uint2 gFragmentPixel;
static bool gFragmentShadowMask;
static bool gFragmentGiNear;

// which of the frame's traced shadows' slots a light is (see ShadowTraceData), or -1
int TracedShadowSlot(uint lightIndex)
{
	uint4 lights = gShadowTrace[gPushConstants.frameIndex][0].lights;
	return lights.x == lightIndex ? 0 : lights.y == lightIndex ? 1 : lights.z == lightIndex ? 2 : lights.w == lightIndex ? 3 : -1;
}

// where a point is in a light's shadow views (see ShadowData): the view (a directional light's nearest cascade whose
// filter stays in its tile, a point light's cube face), and the point's atlas coordinates and depth in it. the point is
// pushed off its surface along the geometry's normal by up to 3 texels of the view (normal offset), against acne, on the
// side facing the light. kShadowMapNone where the light has no views or none covers the point, kShadowMapNearer where it
// is nearer the light than anything its view saw
static const uint kShadowMapPlaced = 0u;
static const uint kShadowMapNone = 1u;
static const uint kShadowMapNearer = 2u;
struct ShadowMapPlace
{
	ShadowData view;
	float2 uv; // in the atlas
	float depth;
};

uint PlaceInShadowMap(uint lightIndex, LightData light, float3 position, float3 geometryNormal, float3 toLight, out ShadowMapPlace place)
{
	place = (ShadowMapPlace)0;
	uint frameIndex = gPushConstants.frameIndex;
	uint first = gLightShadows[frameIndex][lightIndex];
	if (first == SHADER_TYPES_NO_SHADOW)
		return kShadowMapNone;

	uint viewIndex = first;
	if (light.type == LIGHT_TYPE_POINT)
	{
		// the cube face the point is in
		float3 d = position - light.positionRange.xyz;
		float3 a = abs(d);
		uint face = a.x >= a.y && a.x >= a.z ? (d.x >= 0.0 ? 0u : 1u) : a.y >= a.z ? (d.y >= 0.0 ? 2u : 3u) : (d.z >= 0.0 ? 4u : 5u);
		viewIndex = first + face;
	}

	Texture2D atlas = gTextures[SHADER_TYPES_SHADOW_ATLAS_TEXTURE];
	float2 atlasSize;
	atlas.GetDimensions(atlasSize.x, atlasSize.y);
	float2 texel = 1.0 / atlasSize;

	uint viewCount = light.type == LIGHT_TYPE_DIRECTIONAL ? SHADER_TYPES_SHADOW_CASCADE_COUNT : 1u;
	for (uint cascade = 0; cascade < viewCount; cascade++)
	{
		ShadowData shadow = gShadowViews[frameIndex][viewIndex + cascade];
		float texelSize = shadow.params.x;
		if (shadow.params.y > 0.0)
			texelSize *= length(position - light.positionRange.xyz);
		// far enough for the filter's reach (2 texels) at the angle the surface makes with the light, off the side facing
		// it: a surface lit from behind (diffuse transmission, the scattering's light from behind) pushed the other way was
		// behind itself as the light sees it, and shadowed its own transmitted light in the pattern of the point light's
		// cube faces (a star where they meet)
		float nDotL = dot(geometryNormal, toLight);
		float3 offsetNormal = nDotL >= 0.0 ? geometryNormal : -geometryNormal;
		nDotL = min(abs(nDotL), 1.0);
		float3 offsetPosition = position + offsetNormal * (texelSize * (1.0 + 2.0 * sqrt(1.0 - nDotL * nDotL)));
		float4 clip = mul(shadow.viewProjection, float4(offsetPosition, 1.0));
		if (clip.w <= 0.0)
			continue;
		float3 ndc = clip.xyz / clip.w;
		float2 local = ndc.xy * 0.5 + 0.5;
		// a cascade covers the point if its filter stays inside the tile: else the next (farther) one
		float2 margin = 2.5 * texel / shadow.atlasRect.zw;
		if (any(local < margin) || any(local > 1.0 - margin) || ndc.z > 1.0)
			continue;
		if (ndc.z < 0.0)
			return kShadowMapNearer;
		place.view = shadow;
		place.uv = shadow.atlasRect.xy + local * shadow.atlasRect.zw;
		place.depth = ndc.z;
		return kShadowMapPlaced;
	}
	return kShadowMapNone;
}

// the share of a light's atlas texels around a point (atlas coordinates) that see past it: 4x4 texels, as 3x3 bilinear
// lookups of four comparisons each (percentage closer filtering)
float ShadowMapFilter(float2 uv, float depth)
{
	Texture2D atlas = gTextures[SHADER_TYPES_SHADOW_ATLAS_TEXTURE];
	float2 atlasSize;
	atlas.GetDimensions(atlasSize.x, atlasSize.y);
	float2 texel = 1.0 / atlasSize;
	float lit = 0.0;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			float2 st = (uv + float2(x, y) * texel) * atlasSize - 0.5;
			float2 base = floor(st);
			float2 f = st - base;
			// the four texels around: x (0, 1), y (1, 1), z (1, 0), w (0, 0)
			float4 stored = atlas.GatherRed(gSamplers[SHADER_TYPES_CLAMP_SAMPLER], (base + 1.0) * texel);
			float4 visible = select(stored >= depth, float4(1.0), float4(0.0));
			lit += lerp(lerp(visible.w, visible.z, f.x), lerp(visible.x, visible.y, f.x), f.y);
		}
	return lit / 9.0;
}

// the meters a texel of a shadow view covers at a distance from its light (along its view, for a perspective one)
float ShadowTexelSize(ShadowData view, float toLightDistance)
{
	return view.params.x * (view.params.y > 0.0 ? toLightDistance : 1.0);
}

// how the depth of a point's tangent plane changes across the atlas (per atlas coordinate) where its view sees it: two
// points on the plane a texel's size from it, projected. a texel compared with the plane where the texel is, rather
// than with the point's depth, isn't the surface's own blocker where it slopes toward the light
float2 ShadowReceiverGradient(ShadowMapPlace place, float3 position, float3 normal, float texelSize)
{
	float3 t;
	float3 b;
	ShadowBasis(normal, t, b);
	float3 placeAt[3] = {position, position + t * texelSize, position + b * texelSize};
	float3 projected[3];
	for (uint i = 0; i < 3; i++)
	{
		float4 clip = mul(place.view.viewProjection, float4(placeAt[i], 1.0));
		float3 ndc = clip.xyz / max(clip.w, 1e-6);
		projected[i] = float3(place.view.atlasRect.xy + (ndc.xy * 0.5 + 0.5) * place.view.atlasRect.zw, ndc.z);
	}
	float2 du = projected[1].xy - projected[0].xy;
	float2 dv = projected[2].xy - projected[0].xy;
	float determinant = du.x * dv.y - du.y * dv.x;
	if (abs(determinant) <= 1e-12)
		return 0.0;
	float dzU = projected[1].z - projected[0].z;
	float dzV = projected[2].z - projected[0].z;
	return float2(dzU * dv.y - dzV * du.y, dzV * du.x - dzU * dv.x) / determinant;
}

static const uint kShadowSoftTapCount = 16u;
static const float kShadowSoftMostTexels = 16.0;

// the shadow of a light of a size (LightData::size) from its atlas, where its rays aren't traced (the lights past the
// traced ones, the fragments that don't take the traced shadows, a model without a BVH): percentage closer soft shadows
// (Fernando 2005). the blockers around the point within the widest penumbra it can have (a blocker at its view's near
// plane), each compared with the point's tangent plane where it is (ShadowReceiverGradient), the penumbra their mean
// distance makes (ShadowPenumbraHalfWidth, as the traced shadows' filter has it), and the share of the texels within that
// which see past the point: bilinear comparisons at a Vogel spiral of taps turned per pixel (interleaved gradient
// noise; the blockers' taps aren't, their mean distance being all they give), at most 16 texels out. a light without a
// size, or a penumbra under a texel and a half, takes the 3x3 filter. it costs Sponza's inside and Bistro's street 2.7
// to 2.9 ms a frame more than the 3x3 filter where nothing is traced (19.3 against 16.7 ms; 26 against 23.1), the
// blockers' taps most of it
float ShadowMapSoft(ShadowMapPlace place, LightData light, float3 position, float3 geometryNormal)
{
	float toLightDistance = light.type == LIGHT_TYPE_DIRECTIONAL ? 1e9 : length(light.positionRange.xyz - position);
	float texelSize = ShadowTexelSize(place.view, toLightDistance);
	float planeDistance = ShadowViewDistance(place.view, place.depth);
	float widest = ShadowPenumbraHalfWidth(light, max(planeDistance - place.view.params.z, 0.0), toLightDistance) / texelSize;
	if (light.size <= 0.0 || widest < 1.5)
		return ShadowMapFilter(place.uv, place.depth);

	Texture2D atlas = gTextures[SHADER_TYPES_SHADOW_ATLAS_TEXTURE];
	float2 atlasSize;
	atlas.GetDimensions(atlasSize.x, atlasSize.y);
	float2 texel = 1.0 / atlasSize;
	float2 low = place.view.atlasRect.xy + texel;
	float2 high = place.view.atlasRect.xy + place.view.atlasRect.zw - texel;
	float2 gradient = ShadowReceiverGradient(place, position, geometryNormal, texelSize);
	float slack = abs(dot(abs(gradient), texel)) * 2.0 + 1e-5;
	float turn = frac(52.9829189 * frac(dot(float2(gFragmentPixel), float2(0.06711056, 0.00583715)))) * 6.28318530718;

	float searchRadius = min(widest, kShadowSoftMostTexels);
	float blockerSum = 0.0;
	float blockerCount = 0.0;
	for (uint tapIt = 0; tapIt < kShadowSoftTapCount; tapIt++)
	{
		float radius = searchRadius * sqrt((float(tapIt) + 0.5) / float(kShadowSoftTapCount));
		float angle = float(tapIt) * 2.39996323;
		float2 uv = clamp(place.uv + radius * float2(cos(angle), sin(angle)) * texel, low, high);
		float stored = atlas.Load(int3(int2(uv * atlasSize), 0)).r;
		float plane = place.depth + dot(gradient, uv - place.uv);
		if (stored < plane - slack)
		{
			blockerSum += max(ShadowViewDistance(place.view, plane) - ShadowViewDistance(place.view, stored), 0.0);
			blockerCount += 1.0;
		}
	}
	// none of the taps blocking within the widest penumbra the point can have: lit; all of them: in the umbra (most of a
	// shadowed interior, where the filter after cost Sponza's inside 4 ms a frame)
	if (blockerCount <= 0.0)
		return 1.0;
	if (blockerCount >= float(kShadowSoftTapCount))
		return 0.0;
	float filterRadius = min(ShadowPenumbraHalfWidth(light, blockerSum / blockerCount, toLightDistance) / texelSize, kShadowSoftMostTexels);
	if (filterRadius < 1.5)
		return ShadowMapFilter(place.uv, place.depth);

	float lit = 0.0;
	for (uint tapIt = 0; tapIt < kShadowSoftTapCount; tapIt++)
	{
		float radius = filterRadius * sqrt((float(tapIt) + 0.5) / float(kShadowSoftTapCount));
		float angle = float(tapIt) * 2.39996323 + turn;
		float2 uv = clamp(place.uv + radius * float2(cos(angle), sin(angle)) * texel, low, high);
		float2 st = uv * atlasSize - 0.5;
		float2 base = floor(st);
		float2 f = st - base;
		// the four texels around: x (0, 1), y (1, 1), z (1, 0), w (0, 0), each against the plane where it is
		float2 corner = (base + 0.5) * texel - place.uv;
		float4 plane = place.depth + float4(
			dot(gradient, corner + float2(0.0, texel.y)), dot(gradient, corner + texel), dot(gradient, corner + float2(texel.x, 0.0)),
			dot(gradient, corner));
		float4 stored = atlas.GatherRed(gSamplers[SHADER_TYPES_CLAMP_SAMPLER], (base + 1.0) * texel);
		float4 visible = select(stored >= plane - slack, float4(1.0), float4(0.0));
		lit += lerp(lerp(visible.w, visible.z, f.x), lerp(visible.x, visible.y, f.x), f.y);
	}
	return lit / float(kShadowSoftTapCount);
}

// how much of a light reaches a point: the traced shadows' where the fragment takes them (filtered over the penumbra's
// width, see ComputeShadowDenoiseY), else its shadow views' (see PlaceInShadowMap, ShadowMapSoft): 1 without any
float ShadowFactor(uint lightIndex, LightData light, float3 position, float3 geometryNormal, float3 toLight)
{
	if (gPushConstants.shadows == 0u)
		return 1.0;
	if (gFragmentShadowMask)
	{
		int slot = TracedShadowSlot(lightIndex);
		if (slot >= 0)
			return gTextures[SHADER_TYPES_SHADOW_MASK_TEXTURE].Load(int3(int2(gFragmentPixel), 0))[slot];
	}
	ShadowMapPlace place;
	if (PlaceInShadowMap(lightIndex, light, position, geometryNormal, toLight, place) != kShadowMapPlaced)
		return 1.0;
	return ShadowMapSoft(place, light, position, geometryNormal);
}
