// the punctual lights' shadows from the atlas: ShadowFactor. a part of shaders.slang, which includes it after capi.h and sky.h

// how much of a light reaches a point, from its shadow views (see ShadowData): 1 without any. the point is pushed off its
// surface along the geometry's normal by up to 3 texels of the view it is looked up in (normal offset), against acne,
// and filtered over 4x4 texels (percentage closer filtering: 3x3 bilinear lookups, each four comparisons)
float ShadowFactor(uint lightIndex, LightData light, float3 position, float3 geometryNormal, float3 toLight)
{
	if (gPushConstants.shadows == 0u)
		return 1.0;
	uint frameIndex = gPushConstants.frameIndex;
	uint first = gLightShadows[frameIndex][lightIndex];
	if (first == SHADER_TYPES_NO_SHADOW)
		return 1.0;

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
		// far enough for the filter's reach (2 texels) at the angle the surface makes with the light
		float nDotL = saturate(dot(geometryNormal, toLight));
		float3 offsetPosition = position + geometryNormal * (texelSize * (1.0 + 2.0 * sqrt(1.0 - nDotL * nDotL)));
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
			return 1.0; // nearer the light than anything the view saw

		float2 uv = shadow.atlasRect.xy + local * shadow.atlasRect.zw;
		float lit = 0.0;
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++)
			{
				float2 st = (uv + float2(x, y) * texel) * atlasSize - 0.5;
				float2 base = floor(st);
				float2 f = st - base;
				// the four texels around: x (0, 1), y (1, 1), z (1, 0), w (0, 0)
				float4 stored = atlas.GatherRed(gSamplers[SHADER_TYPES_CLAMP_SAMPLER], (base + 1.0) * texel);
				float4 visible = select(stored >= ndc.z, float4(1.0), float4(0.0));
				lit += lerp(lerp(visible.w, visible.z, f.x), lerp(visible.x, visible.y, f.x), f.y);
			}
		return lit / 9.0;
	}
	return 1.0;
}
