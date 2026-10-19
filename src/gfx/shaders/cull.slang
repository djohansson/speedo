// the gpu's culling of the frame's draws (the Cull pass): a thread per draw and view that draws it (see CullCandidate)

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

// a draw in view takes the next place of its run's commands (in any order: opaque draws depth test, and blended ones
// are order independent)
[shader("compute")]
[numthreads(SHADER_TYPES_CULL_GROUP_SIZE, 1, 1)]
void ComputeCull(uint3 threadId: SV_DispatchThreadID)
{
	uint frameIndex = gPushConstants.frameIndex;
	CullJob job = gCullJobs[frameIndex][threadId.y];
	if (threadId.x >= job.candidateCount)
		return;

	CullCandidate candidate = gCullCandidates[frameIndex][job.candidateBase + threadId.x];
	if (candidate.boundsMin.w != 0.0 && CullExcludes(gCullViews[frameIndex][job.view], candidate.boundsMin.xyz, candidate.boundsMax.xyz))
		return;

	uint slot;
	InterlockedAdd(gCullCounts[frameIndex][job.countBase + candidate.run], 1, slot);
	DrawIndexedIndirectCommand command;
	command.indexCount = candidate.indexCount;
	command.instanceCount = candidate.instanceCount;
	command.firstIndex = candidate.firstIndex;
	command.vertexOffset = 0;
	command.firstInstance = candidate.record;
	gCullCommands[frameIndex][job.commandBase + candidate.runFirst + slot] = command;
}
