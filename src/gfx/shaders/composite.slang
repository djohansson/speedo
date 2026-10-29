// ComputeMain: the frame's composite (transparency, backdrop, exposure, tonemapping) into the output. a part of shaders.slang, which includes it after capi.h and sky.h

// the group's share of the frame's histograms (the exposure's, then the distances'), added to gExposureHistogram once
// the group is done
groupshared uint gGroupHistogram[SHADER_TYPES_HISTOGRAM_BINS];

[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeMain(
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID,
	uint3 dispatchThreadId : SV_DispatchThreadID,
	uint groupIndex : SV_GroupIndex)
{
	uint frameIndex = gPushConstants.frameIndex;

	Texture2D src = gTextures[SHADER_TYPES_RENDER_TARGET_TEXTURE];
	// with temporal upscaling (UPSCALE_COMPOSITE), the composite at the render size, linear and before exposure, which the
	// Upscale passes reconstruct at the output's size (see upscale.slang)
	bool composite = (gPushConstants.upscale & UPSCALE_COMPOSITE) != 0u;
	RWTexture2D dst = gRWTextures[composite ? SHADER_TYPES_COMPOSITE_RW_TEXTURE : gPushConstants.outputIndex];

	uint2 dstDim;
	dst.GetDimensions(dstDim.x, dstDim.y);

	if (groupIndex < SHADER_TYPES_HISTOGRAM_BINS)
		gGroupHistogram[groupIndex] = 0;
	GroupMemoryBarrierWithGroupSync();

	// a thread per pixel: a thread per 16x16 pixels, as it was, ran too few threads to hide its reads' latency
	uint x = dispatchThreadId.x;
	uint y = dispatchThreadId.y;
	if (x < dstDim.x && y < dstDim.y)
	{
		uint2 xy = uint2(x, y);

		// the render target is linear, the swapchain (a storage image, so not an srgb format) is not: exposed,
		// tonemapped, and then the srgb curve
		float4 color = src.Load(int3(xy, 0));

		// the environment where nothing opaque was drawn, in the view the pixel is in (outside them, e.g. where a view
		// is letterboxed, or without the backdrop, a gray): along the ray through the pixel, between two depths of it
		// a debug view of values (see DebugViewIsData) is shown as it is, over a dark backdrop
		bool data = DebugViewIsData(gPushConstants.debugView);
		if (color.a == 0.0)
			color.rgb = data ? 0.005 : 0.2;
		if (color.a == 0.0 && gPushConstants.environmentBackdrop != 0u && !data)
		{
			for (uint viewIt = 0; viewIt < gPushConstants.viewCount; viewIt++)
			{
				ViewData view = gViewData[frameIndex][viewIt];
				float2 local = (float2(xy) + 0.5 - view.viewport.xy) / view.viewport.zw;
				if (any(local < 0.0) || any(local >= 1.0))
					continue;
				float2 ndc = local * 2.0 - 1.0;
				float4 nearPoint = mul(view.inverseViewProjection, float4(ndc, 1.0, 1.0)); // the near plane: depth reversed
				float4 farPoint = mul(view.inverseViewProjection, float4(ndc, 0.5, 1.0));
				float3 ray = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
				color.rgb = EnvironmentRadiance(gEnvironment[0], ray, 0.0);
				break;
			}
		}

		// the voxel world (DEBUG_VIEW_GI_VOXEL_WORLD*): every pixel's ray through the voxels, in place of what was drawn
		uint debugView = gPushConstants.debugView;
		bool voxelWorld = debugView >= DEBUG_VIEW_GI_VOXEL_WORLD && debugView <= DEBUG_VIEW_GI_VOXEL_WORLD_NORMAL;
		if (voxelWorld)
		{
			ViewData view;
			if (ViewAt(int2(xy), view))
			{
				float2 local = (float2(xy) + 0.5 - view.viewport.xy) / view.viewport.zw;
				float2 ndc = local * 2.0 - 1.0;
				float4 nearPoint = mul(view.inverseViewProjection, float4(ndc, 1.0, 1.0)); // the near plane: depth reversed
				float4 farPoint = mul(view.inverseViewProjection, float4(ndc, 0.5, 1.0));
				float3 ray = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
				color.rgb = GiVoxelWorldDebug(view.eyePosition.xyz, ray, debugView);
			}
		}

		// how much of the pixel the blended layers cover, temporal upscaling's reactive mask (see UpscaleReactiveMask): they
		// draw no motion of their own, so their history would follow what is behind them
		float coverage = 0.0;
		// the blended fragments over the rest: the pixel's list, its nearest layers sorted near to far (by insertion),
		// then blended far to near
		uint node = gOitHeads[y * gPushConstants.framebufferWidth + x];
		if (node != SHADER_TYPES_OIT_NONE && !voxelWorld)
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
				coverage = coverage + alpha - coverage * alpha;
			}
		}

		// auto exposure counts the scene's luminance, before exposure
		float luminance = dot(max(color.rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
		float position = (log2(max(luminance, 1e-8)) - SHADER_TYPES_EXPOSURE_LOG2_MIN) /
						 (SHADER_TYPES_EXPOSURE_LOG2_MAX - SHADER_TYPES_EXPOSURE_LOG2_MIN);
		uint bin = uint(clamp(position * float(SHADER_TYPES_EXPOSURE_BINS), 0.0, float(SHADER_TYPES_EXPOSURE_BINS - 1)));
		InterlockedAdd(gGroupHistogram[bin], 1u);
		// and how far what the pixel shows is from the eye, where something was drawn (the main depth unprojected in
		// the pixel's view)
		if (color.a != 0.0)
		{
			ViewData view;
			if (ViewAt(int2(xy), view))
			{
				float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(xy, 0)).r;
				float2 local = (float2(xy) + 0.5 - view.viewport.xy) / view.viewport.zw;
				float4 world = mul(view.inverseViewProjection, float4(local * 2.0 - 1.0, depth, 1.0));
				// the world position under the pixel dumped, for the indirect lighting's dump (see GiData::dumpPixel)
				float4 dumpPixel = gGi[frameIndex][0].dumpPixel;
				if (dumpPixel.w != 0.0 && all(xy == uint2(dumpPixel.xy)))
				{
					float3 at = world.xyz / world.w;
					gGiStatistics[frameIndex][GI_STATISTIC_DUMP_POINT] = asuint(at.x);
					gGiStatistics[frameIndex][GI_STATISTIC_DUMP_POINT + 1] = asuint(at.y);
					gGiStatistics[frameIndex][GI_STATISTIC_DUMP_POINT + 2] = asuint(at.z);
					gGiStatistics[frameIndex][GI_STATISTIC_DUMP_POINT + 3] = 1u;
				}
				// the indirect lighting's blocks the pixel's lookup reads, for its surface cache (see GiMarkSeen)
				float3 normal = gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(xy, 0)).xyz * 2.0 - 1.0;
				if (dot(normal, normal) > 0.25)
					GiMarkSeen(world.xyz / world.w, normalize(normal), normalize(view.eyePosition.xyz - world.xyz / world.w));
				float distance = length(world.xyz / world.w - view.eyePosition.xyz);
				float at = (log2(max(distance, 1e-8)) - SHADER_TYPES_DISTANCE_LOG2_MIN) / (SHADER_TYPES_DISTANCE_LOG2_MAX - SHADER_TYPES_DISTANCE_LOG2_MIN);
				uint distanceBin = uint(clamp(at * float(SHADER_TYPES_DISTANCE_BINS), 0.0, float(SHADER_TYPES_DISTANCE_BINS - 1)));
				InterlockedAdd(gGroupHistogram[SHADER_TYPES_EXPOSURE_BINS + distanceBin], 1u);
			}
		}

		if (composite)
			dst.Store(xy, float4(max(color.rgb, 0.0), coverage));
		else
		{
			float3 mapped = data ? max(color.rgb, 0.0) : Tonemap(max(color.rgb, 0.0) * gPushConstants.exposure);
			dst.Store(xy, float4(ApplySRGBCurve(saturate(mapped)), 1.0));

			// the ui's frosted panels' backdrop: under them and within the blur's reach, linear, for the UI Blur passes
			if (gPushConstants.uiBlurRectCount != 0u && UiBlurReaches(xy))
				gRWTextures[SHADER_TYPES_UI_BLUR_SOURCE_RW_TEXTURE].Store(xy, float4(saturate(mapped), 1.0));
		}
	}

	GroupMemoryBarrierWithGroupSync();
	if (groupIndex < SHADER_TYPES_HISTOGRAM_BINS && gGroupHistogram[groupIndex] > 0)
		InterlockedAdd(gExposureHistogram[frameIndex * SHADER_TYPES_HISTOGRAM_BINS + groupIndex], gGroupHistogram[groupIndex]);
}
