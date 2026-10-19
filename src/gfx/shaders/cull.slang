// the gpu's culling of the frame's draws (the Cull passes): a thread per draw and view that draws it (see CullCandidate),
// and the depth pyramid occlusion is tested against (the Depth Pyramid pass)

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

	uint width;
	uint height;
	gDepthPyramid[0].GetDimensions(width, height);
	uint levelCount = 1;
	while (levelCount < SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT && ((width >> levelCount) > 0 || (height >> levelCount) > 0))
		levelCount++;

	// a texel of level n covers (at least) 2^(n + 1) pixels each way: level 0 is half the main depth's size
	float extent = max(max(screenMax.x - screenMin.x, screenMax.y - screenMin.y), 1.0);
	uint level = min(uint(max(ceil(log2(extent)), 1.0)) - 1, levelCount - 1);
	uint2 levelSize = max(uint2(width, height) >> level, uint2(1));
	uint2 first = min(uint2(screenMin) >> (level + 1), levelSize - 1);
	uint2 last = min(uint2(screenMax) >> (level + 1), levelSize - 1);
	float farthest = 0.0;
	for (uint y = first.y; y <= last.y; y++)
		for (uint x = first.x; x <= last.x; x++)
			farthest = max(farthest, gDepthPyramid[NonUniformResourceIndex(level)][uint2(x, y)]);
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
		break;
	}
	case CULL_JOB_PYRAMID:
		keep = inFrustum && (!cullable || !CullOccluded(view, candidate.boundsMin.xyz, candidate.boundsMax.xyz));
		break;
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
	uint level = gPushConstants.depthPyramidLevel;
	uint width;
	uint height;
	gDepthPyramid[NonUniformResourceIndex(level)].GetDimensions(width, height);
	if (threadId.x >= width || threadId.y >= height)
		return;

	float farthest = 0.0;
	uint sourceWidth;
	uint sourceHeight;
	if (level == 0)
		gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].GetDimensions(sourceWidth, sourceHeight);
	else
		gDepthPyramid[NonUniformResourceIndex(level - 1)].GetDimensions(sourceWidth, sourceHeight);
	for (uint y = 0; y < 3; y++)
		for (uint x = 0; x < 3; x++)
		{
			uint2 source = min(threadId.xy * 2 + uint2(x, y), uint2(sourceWidth, sourceHeight) - 1);
			farthest = max(
				farthest,
				level == 0 ? gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(source, 0)).r
						   : gDepthPyramid[NonUniformResourceIndex(level - 1)][source]);
		}
	gDepthPyramid[NonUniformResourceIndex(level)][threadId.xy] = farthest;
}
