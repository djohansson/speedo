// the gpu's culling of the frame's draws (the Cull passes): a thread per draw and view that draws it (see CullCandidate),
// and the depth pyramid occlusion is tested against (the Depth Pyramid pass)

// a depth pyramid's (CullView::pyramid) level's size, and texel
uint2 PyramidSize(uint pyramid, uint level)
{
	uint width;
	uint height;
	if (pyramid == 0)
		gDepthPyramid[NonUniformResourceIndex(level)].GetDimensions(width, height);
	else
		gShadowDepthPyramid[NonUniformResourceIndex(level)].GetDimensions(width, height);
	return uint2(width, height);
}

float PyramidTexel(uint pyramid, uint level, uint2 texel)
{
	return pyramid == 0 ? gDepthPyramid[NonUniformResourceIndex(level)][texel] : gShadowDepthPyramid[NonUniformResourceIndex(level)][texel];
}

// whether a box is entirely outside one of a view's planes (as gfx::Frustum::Excludes)
bool CullExcludes(CullView view, float3 boundsMin, float3 boundsMax)
{
	[unroll]
	for (uint planeIt = 0; planeIt < 6; planeIt++)
	{
		float4 plane = view.planes[planeIt];
		// the corner furthest along the plane's normal
		float3 corner = select(plane.xyz >= 0.0, boundsMax, boundsMin);
		if (dot(plane.xyz, corner) + plane.w < 0.0)
			return true;
	}
	return false;
}

// whether the depth pyramid hides a box from a view: its nearest depth is farther than the farthest of the pyramid's
// texels under its rectangle on screen, at the level where that rectangle spans at most two texels each way. a box
// reaching behind the eye (or in front of the near plane) is never hidden
bool CullOccluded(CullView view, float3 boundsMin, float3 boundsMax)
{
	if (view.occlusion == 0)
		return false;

	float2 screenMin = float2(1.0e30);
	float2 screenMax = float2(-1.0e30);
	float nearest = 1.0;
	[unroll]
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		float3 corner = float3(
			(cornerIt & 1) != 0 ? boundsMax.x : boundsMin.x,
			(cornerIt & 2) != 0 ? boundsMax.y : boundsMin.y,
			(cornerIt & 4) != 0 ? boundsMax.z : boundsMin.z);
		float4 clip = mul(view.viewProjection, float4(corner, 1.0));
		if (clip.w <= 1.0e-6 || clip.z < 0.0)
			return false;
		float3 ndc = clip.xyz / clip.w;
		float2 pixel = view.viewport.xy + (ndc.xy * 0.5 + 0.5) * view.viewport.zw;
		screenMin = min(screenMin, pixel);
		screenMax = max(screenMax, pixel);
		nearest = min(nearest, ndc.z);
	}
	screenMin = max(screenMin, view.viewport.xy);
	screenMax = min(screenMax, view.viewport.xy + view.viewport.zw - 1.0);
	if (any(screenMax < screenMin))
		return false; // off the view: the frustum test keeps it, conservatively

	uint2 size = PyramidSize(view.pyramid, 0);
	uint levelCount = 1;
	uint maxLevelCount = view.pyramid == 0 ? SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT : SHADER_TYPES_SHADOW_DEPTH_PYRAMID_LEVEL_COUNT;
	while (levelCount < maxLevelCount && ((size.x >> levelCount) > 0 || (size.y >> levelCount) > 0))
		levelCount++;

	// a texel of level n covers (at least) 2^(n + 1) pixels each way: level 0 is half its source's size. a shadow view
	// reads no level coarser than its tile (whose texels would be its neighbors' too): its rectangle may span more texels
	float extent = max(max(screenMax.x - screenMin.x, screenMax.y - screenMin.y), 1.0);
	uint level = min(min(uint(max(ceil(log2(extent)), 1.0)) - 1, levelCount - 1), view.maxLevel);
	uint2 levelSize = max(size >> level, uint2(1));
	uint2 first = min(uint2(screenMin) >> (level + 1), levelSize - 1);
	uint2 last = min(uint2(screenMax) >> (level + 1), levelSize - 1);
	float farthest = 0.0;
	for (uint y = first.y; y <= last.y; y++)
		for (uint x = first.x; x <= last.x; x++)
			farthest = max(farthest, PyramidTexel(view.pyramid, level, uint2(x, y)));
	return nearest > farthest;
}

// a draw a job keeps takes the next place of its run's commands (in any order: opaque draws depth test, and blended ones
// are order independent)
[shader("compute")]
[numthreads(SHADER_TYPES_CULL_GROUP_SIZE, 1, 1)]
void ComputeCull(uint3 threadId: SV_DispatchThreadID)
{
	uint frameIndex = gPushConstants.frameIndex;
	CullJob job = gCullJobs[frameIndex][gPushConstants.cullJobBase + threadId.y];
	if (threadId.x >= job.candidateCount)
		return;

	CullCandidate candidate = gCullCandidates[frameIndex][job.candidateBase + threadId.x];
	CullView view = gCullViews[frameIndex][job.view];
	bool cullable = candidate.boundsMin.w != 0.0;
	bool inFrustum = !cullable || !CullExcludes(view, candidate.boundsMin.xyz, candidate.boundsMax.xyz);
	uint visibility = job.visibilityBase + threadId.x;

	bool keep = false;
	bool hidden = false; // in view, and hidden
	switch (job.kind)
	{
	case CULL_JOB_FRUSTUM:
		keep = inFrustum;
		break;
	case CULL_JOB_EARLY:
		keep = inFrustum && candidate.late == 0 && gCullVisibility[visibility] != 0;
		break;
	case CULL_JOB_LATE:
	{
		bool drawnEarly = candidate.late == 0 && gCullVisibility[visibility] != 0 && inFrustum;
		bool seen = inFrustum && (!cullable || !CullOccluded(view, candidate.boundsMin.xyz, candidate.boundsMax.xyz));
		gCullVisibility[visibility] = seen ? 1 : 0;
		keep = seen && !drawnEarly;
		hidden = inFrustum && !seen;
		break;
	}
	case CULL_JOB_PYRAMID:
		keep = inFrustum && (!cullable || !CullOccluded(view, candidate.boundsMin.xyz, candidate.boundsMax.xyz));
		hidden = inFrustum && !keep;
		break;
	case CULL_JOB_HYBRID_EARLY:
		keep = inFrustum && candidate.late == 0 && (!cullable || !CullOccluded(view, candidate.boundsMin.xyz, candidate.boundsMax.xyz));
		gCullVisibility[visibility] = keep ? 1 : 0;
		break;
	case CULL_JOB_HYBRID_LATE:
		keep = inFrustum && gCullVisibility[visibility] == 0 &&
			   (!cullable || !CullOccluded(view, candidate.boundsMin.xyz, candidate.boundsMax.xyz));
		hidden = inFrustum && gCullVisibility[visibility] == 0 && !keep;
		break;
	}
	if (job.kind != CULL_JOB_FRUSTUM && (keep || hidden))
	{
		bool late = job.kind == CULL_JOB_LATE || job.kind == CULL_JOB_HYBRID_LATE;
		uint statistic = hidden ? CULL_STATISTIC_HIDDEN
						 : !late ? CULL_STATISTIC_EARLY
								 : (candidate.late != 0 ? CULL_STATISTIC_LATE_BLENDED : CULL_STATISTIC_LATE);
		InterlockedAdd(gCullStatistics[frameIndex][job.statistics * CULL_STATISTIC_COUNT + statistic], 1);
	}
	if (!keep && job.compact != 0)
		return;

	uint place = threadId.x; // the draw's own
	if (job.compact != 0)
	{
		uint slot;
		InterlockedAdd(gCullCounts[frameIndex][job.countBase + candidate.run], 1, slot);
		place = candidate.runFirst + slot;
	}
	DrawIndexedIndirectCommand command;
	command.indexCount = candidate.indexCount;
	command.instanceCount = keep ? candidate.instanceCount : 0;
	command.firstIndex = candidate.firstIndex;
	command.vertexOffset = 0;
	command.firstInstance = candidate.record;
	gCullCommands[frameIndex][job.commandBase + place] = command;
}

// a level of the depth pyramid: each the farthest of the 3x3 texels of the one below (the main depth for level 0, which
// is half its size, rounded up) from twice its texel: one more than the 2x2 a texel halves, so that an odd size's last
// texels are covered (it only makes the pyramid farther, which hides less)
[shader("compute")]
[numthreads(SHADER_TYPES_DEPTH_PYRAMID_GROUP_SIZE, SHADER_TYPES_DEPTH_PYRAMID_GROUP_SIZE, 1)]
void ComputeDepthPyramid(uint3 threadId: SV_DispatchThreadID)
{
	// the main views' pyramid from the main depth (source 0) or the depth reprojected (1), the shadow views' from the atlas
	// (2)
	uint level = gPushConstants.depthPyramidLevel;
	uint sourceKind = gPushConstants.depthPyramidSource;
	uint pyramid = sourceKind == 2 ? 1 : 0;
	uint2 size = PyramidSize(pyramid, level);
	if (threadId.x >= size.x || threadId.y >= size.y)
		return;

	float farthest = 0.0;
	uint2 sourceSize = uint2(gPushConstants.framebufferWidth, gPushConstants.framebufferHeight);
	if (level > 0)
		sourceSize = PyramidSize(pyramid, level - 1);
	else if (sourceKind == 2)
		gTextures[SHADER_TYPES_SHADOW_ATLAS_TEXTURE].GetDimensions(sourceSize.x, sourceSize.y);
	for (uint y = 0; y < 3; y++)
		for (uint x = 0; x < 3; x++)
		{
			uint2 source = min(threadId.xy * 2 + uint2(x, y), sourceSize - 1);
			float depth;
			if (level > 0)
				depth = PyramidTexel(pyramid, level - 1, source);
			else if (sourceKind == 1)
				depth = asfloat(gReprojectedDepth[source.y * sourceSize.x + source.x]);
			else
				depth = gTextures[sourceKind == 2 ? SHADER_TYPES_SHADOW_ATLAS_TEXTURE : SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(source, 0)).r;
			farthest = max(farthest, depth);
		}
	if (pyramid == 0)
		gDepthPyramid[NonUniformResourceIndex(level)][threadId.xy] = farthest;
	else
		gShadowDepthPyramid[NonUniformResourceIndex(level)][threadId.xy] = farthest;
}

// the depth reprojection: each frame saves its depth of what doesn't move (ComputeDepthSave; what moves is at the far
// plane, its depth last frame's place), and the next pushes each saved pixel to where its point is seen now
// (ComputeDepthReproject: its view's last inverse view projection, then the view projection now), the nearest of those
// landing on a pixel kept. every point pushed is on a surface that doesn't move, so it is no nearer than what the pixel
// sees now (pixels nothing lands on stay at the far plane, as the gaps when coming closer): conservative but for
// rounding, which a little bias farther and the pyramid's 3x3 at level 0 make up for
[shader("compute")]
[numthreads(SHADER_TYPES_DEPTH_REPROJECT_GROUP_SIZE, SHADER_TYPES_DEPTH_REPROJECT_GROUP_SIZE, 1)]
void ComputeDepthSave(uint3 threadId: SV_DispatchThreadID)
{
	uint width = gPushConstants.framebufferWidth;
	if (threadId.x >= width || threadId.y >= gPushConstants.framebufferHeight)
		return;
	float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(threadId.xy, 0)).r;
	bool moves = (gMainStencil.Load(int3(threadId.xy, 0)) & 1) != 0;
	gPreviousDepth[threadId.y * width + threadId.x] = moves ? 1.0 : depth;
}

[shader("compute")]
[numthreads(SHADER_TYPES_DEPTH_REPROJECT_GROUP_SIZE, SHADER_TYPES_DEPTH_REPROJECT_GROUP_SIZE, 1)]
void ComputeDepthReproject(uint3 threadId: SV_DispatchThreadID)
{
	uint previousWidth = gPushConstants.previousWidth;
	if (threadId.x >= previousWidth || threadId.y >= gPushConstants.previousHeight)
		return;
	float depth = gPreviousDepth[threadId.y * previousWidth + threadId.x];
	if (depth >= 1.0)
		return;

	float2 pixel = float2(threadId.xy) + 0.5;
	for (uint viewIt = 0; viewIt < gPushConstants.mainViewCount; viewIt++)
	{
		CullView view = gCullViews[gPushConstants.frameIndex][viewIt];
		float4 previousViewport = view.previousViewport;
		if (view.previousValid == 0 || any(pixel < previousViewport.xy) || any(pixel >= previousViewport.xy + previousViewport.zw))
			continue;

		float2 ndc = (pixel - previousViewport.xy) / previousViewport.zw * 2.0 - 1.0;
		float4 world = mul(view.previousInverseViewProjection, float4(ndc, depth, 1.0));
		float4 clip = mul(view.viewProjection, float4(world.xyz / world.w, 1.0));
		if (clip.w <= 1.0e-6)
			return;
		float3 now = clip.xyz / clip.w;
		if (now.z < 0.0 || now.z >= 1.0)
			return;
		float2 target = view.viewport.xy + (now.xy * 0.5 + 0.5) * view.viewport.zw;
		if (any(target < view.viewport.xy) || any(target >= view.viewport.xy + view.viewport.zw))
			return;
		float biased = min(now.z + (1.0 - now.z) * 1.0e-3, 1.0);
		uint2 at = uint2(target);
		InterlockedMin(gReprojectedDepth[at.y * gPushConstants.framebufferWidth + at.x], asuint(biased));
		return;
	}
}
