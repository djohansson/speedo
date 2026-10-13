// ComputeMain: the frame's composite (transparency, backdrop, exposure, tonemapping) into the output. a part of shaders.slang, which includes it after capi.h and sky.h

// the group's share of the frame's exposure histogram, added to gExposureHistogram once the group is done
groupshared uint gGroupHistogram[SHADER_TYPES_EXPOSURE_BINS];

[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeMain(
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID,
	uint3 dispatchThreadId : SV_DispatchThreadID,
	uint groupIndex : SV_GroupIndex)
{
	uint frameIndex = gPushConstants.frameIndex;

	Texture2D src = gTextures[SHADER_TYPES_RENDER_TARGET_TEXTURE_BASE + frameIndex];
	RWTexture2D dst = gRWTextures[frameIndex];

	uint2 dstDim;
	dst.GetDimensions(dstDim.x, dstDim.y);

	static const uint2 bucketSize = uint2(16, 16); 

	if (groupIndex < SHADER_TYPES_EXPOSURE_BINS)
		gGroupHistogram[groupIndex] = 0;
	GroupMemoryBarrierWithGroupSync();

	for (uint j = 0; j < bucketSize.x; j++)
	{
		uint y = dispatchThreadId.y * bucketSize.y + j;
		if (y >= dstDim.y)
			break;

		for (uint i = 0; i < bucketSize.y; i++)
		{
			uint x = dispatchThreadId.x * bucketSize.x + i;
			if (x >= dstDim.x)
				break;

			uint2 xy = uint2(x, y);

			// the render target is linear, the swapchain (a storage image, so not an srgb format) is not: exposed,
			// tonemapped, and then the srgb curve
			float4 color = src.Load(int3(xy, 0));

			// the environment where nothing opaque was drawn, in the view the pixel is in (outside them, e.g. where a view
			// is letterboxed, or without the backdrop, a gray): along the ray through the pixel, between two depths of it
			if (color.a == 0.0)
				color.rgb = 0.2;
			if (color.a == 0.0 && gPushConstants.environmentBackdrop != 0u)
			{
				for (uint viewIt = 0; viewIt < gPushConstants.viewCount; viewIt++)
				{
					ViewData view = gViewData[frameIndex][viewIt];
					float2 local = (float2(xy) + 0.5 - view.viewport.xy) / view.viewport.zw;
					if (any(local < 0.0) || any(local >= 1.0))
						continue;
					float2 ndc = local * 2.0 - 1.0;
					float4 nearPoint = mul(view.inverseViewProjection, float4(ndc, 0.0, 1.0));
					float4 farPoint = mul(view.inverseViewProjection, float4(ndc, 0.5, 1.0));
					float3 ray = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
					color.rgb = EnvironmentRadiance(gEnvironment[0], ray, 0.0);
					break;
				}
			}

			// the blended fragments over the rest: the pixel's list, its nearest layers sorted near to far (by insertion),
			// then blended far to near
			uint node = gOitHeads[y * gPushConstants.framebufferWidth + x];
			if (node != SHADER_TYPES_OIT_NONE)
			{
				uint layers[SHADER_TYPES_OIT_MAX_LAYERS];
				uint keys[SHADER_TYPES_OIT_MAX_LAYERS];
				uint layerCount = 0;
				for (uint visited = 0; node != SHADER_TYPES_OIT_NONE && node < gPushConstants.oitNodeCapacity && visited < 4096u; visited++)
				{
					OitNode current = gOitNodes[node];
					uint depth = current.alphaDepth & 0xffffffu;
					if (layerCount < SHADER_TYPES_OIT_MAX_LAYERS || depth < keys[layerCount - 1])
					{
						// past the last kept layer if there is room, else over it (the farthest drops out)
						uint slot = layerCount < SHADER_TYPES_OIT_MAX_LAYERS ? layerCount++ : layerCount - 1;
						while (slot > 0 && keys[slot - 1] > depth)
						{
							keys[slot] = keys[slot - 1];
							layers[slot] = layers[slot - 1];
							slot--;
						}
						keys[slot] = depth;
						layers[slot] = node;
					}
					node = current.next;
				}
				for (uint layerIt = layerCount; layerIt > 0; layerIt--)
				{
					OitNode layer = gOitNodes[layers[layerIt - 1]];
					float alpha = float(layer.alphaDepth >> 24) / 255.0;
					color.rgb = color.rgb * (1.0 - alpha) + UnpackR11G11B10(layer.color);
				}
			}

			// auto exposure counts the scene's luminance, before exposure
			float luminance = dot(max(color.rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
			float position = (log2(max(luminance, 1e-8)) - SHADER_TYPES_EXPOSURE_LOG2_MIN) /
							 (SHADER_TYPES_EXPOSURE_LOG2_MAX - SHADER_TYPES_EXPOSURE_LOG2_MIN);
			uint bin = uint(clamp(position * float(SHADER_TYPES_EXPOSURE_BINS), 0.0, float(SHADER_TYPES_EXPOSURE_BINS - 1)));
			InterlockedAdd(gGroupHistogram[bin], 1u);

			float3 mapped = Tonemap(max(color.rgb, 0.0) * gPushConstants.exposure);
			dst.Store(xy, float4(ApplySRGBCurve(saturate(mapped)), 1.0));
		}
	}

	GroupMemoryBarrierWithGroupSync();
	if (groupIndex < SHADER_TYPES_EXPOSURE_BINS && gGroupHistogram[groupIndex] > 0)
		InterlockedAdd(gExposureHistogram[frameIndex * SHADER_TYPES_EXPOSURE_BINS + groupIndex], gGroupHistogram[groupIndex]);
}
