// the model's triangles traced (BvhTraceInstances and what calls it: BvhTraceScene, BvhTraceGlass, BvhTraceSceneOrGlass):
// by the device's ray queries where it has them (BVH_RAY_QUERY) and through gfx's BVHs (bvh.slang's traversal) where it
// hasn't. a part of shaders.slang, which includes it after bvh.slang (which the tools' kernels include alone, without
// the scene's bindings: src/tools/bvhkernels.slang)

#if BVH_RAY_QUERY
// a ray query's geometry (see gfx/accelerationstructures.h): its entry in gBvhTriangles (its flags, its first index, and
// its instance or first corner, see BVH_RAY_QUERY_MASK_STATIC), by its instance's custom index (its first geometry) and its
// index in its structure
uint4 BvhRayQueryGeometry(uint customIndex, uint geometry)
{
	return asuint(gBvhTriangles[customIndex + geometry]);
}

// whether a top level instance is of what moves (what moves, the glass that moves: its corners as the frame placed them)
bool BvhRayQueryMoving(uint instance)
{
	return instance == BVH_RAY_QUERY_INSTANCE_MOVING || instance == BVH_RAY_QUERY_INSTANCE_MOVING_GLASS;
}

// a ray query's triangle's corners in world space: what doesn't move's vertices placed by its instance, what moves' as the
// frame placed them
void BvhRayQueryCorners(uint instance, uint4 geometry, uint primitive, out float3 corners[3])
{
	uint frameIndex = gPushConstants.frameIndex;
	for (uint cornerIt = 0; cornerIt < 3; cornerIt++)
	{
		if (BvhRayQueryMoving(instance))
			corners[cornerIt] = gBvhMovingTriangles[frameIndex][geometry.z + primitive * 3u + cornerIt].xyz;
		else
			corners[cornerIt] = mul(
				gModelInstances[frameIndex][geometry.z].modelTransform,
				float4(gVertexBuffer[gIndexBuffer[geometry.y + primitive * 3u + cornerIt]].position, 1.0)).xyz;
	}
}

#if BVH_RAY_QUERY_WATCH
// diagnostic (see RAY_QUERY_WATCH_BEGUN): whether the watch records here, in compute shaders only (a fragment shader's
// storage writes, even on a path never taken, turn its early depth test off, see GiMarch's cap)
bool RayQueryWatchHere()
{
	__stage_switch
	{
	case compute:
		return true;
	default:
		return false;
	}
}

// a query begun: counted, and recorded in the slot its ticket names if that is free. its slot's first word, or ~0u
uint RayQueryWatchBegin(float3 origin, float3 direction, float tmax, bool anyHit, bool moving)
{
	if (!RayQueryWatchHere())
		return ~0u;
	uint ticket;
	InterlockedAdd(gRayQueryWatch[RAY_QUERY_WATCH_BEGUN], 1u, ticket);
	uint slot = RAY_QUERY_WATCH_HEADER_COUNT + (ticket % RAY_QUERY_WATCH_SLOT_COUNT) * RAY_QUERY_WATCH_SLOT_WORD_COUNT;
	uint previous;
	InterlockedCompareExchange(gRayQueryWatch[slot], 0u, ticket + 1u, previous);
	if (previous != 0u)
	{
		InterlockedAdd(gRayQueryWatch[RAY_QUERY_WATCH_UNRECORDED], 1u);
		return ~0u;
	}
	uint words[RAY_QUERY_WATCH_SLOT_WORD_COUNT - 1u] = {
		gPushConstants.frameIndex | (anyHit ? 0x100u : 0u) | (moving ? 0x200u : 0u),
		0u,
		asuint(origin.x), asuint(origin.y), asuint(origin.z),
		asuint(direction.x), asuint(direction.y), asuint(direction.z),
		asuint(tmax),
		gPushConstants.giClip | (gPushConstants.giStep << 8u),
		RAY_QUERY_WATCH_STAGE_BEGUN,
	};
	uint ignored;
	for (uint wordIt = 0; wordIt < RAY_QUERY_WATCH_SLOT_WORD_COUNT - 1u; wordIt++)
		InterlockedExchange(gRayQueryWatch[slot + 1u + wordIt], words[wordIt], ignored);
	return slot;
}

// where a query recorded in slot is: its stage, and the candidates it has taken
void RayQueryWatchStage(uint slot, uint stage, uint candidates)
{
	if (slot == ~0u)
		return;
	uint ignored;
	InterlockedExchange(gRayQueryWatch[slot + 2u], candidates, ignored);
	InterlockedExchange(gRayQueryWatch[slot + RAY_QUERY_WATCH_SLOT_WORD_COUNT - 1u], stage, ignored);
}

// a query ended, after it took candidates: counted, and its slot freed
void RayQueryWatchEnd(uint slot, uint candidates)
{
	if (!RayQueryWatchHere())
		return;
	InterlockedAdd(gRayQueryWatch[RAY_QUERY_WATCH_ENDED], 1u);
	InterlockedMax(gRayQueryWatch[RAY_QUERY_WATCH_MAX_CANDIDATES], candidates);
	if (slot == ~0u)
		return;
	uint ignored;
	InterlockedExchange(gRayQueryWatch[slot], 0u, ignored);
}
#endif
#endif

// the model's triangles traced, by the device's ray queries where it has them (BVH_RAY_QUERY: gBvhScene[frame]) and
// through gfx's BVHs where it hasn't (BvhTraverse, BvhTraverse2): one interface, BvhTraceInstances and what calls it
// below, so that what traces compiles and works with either, slower through the BVHs. the BVHs hold the glass among
// the rest, flagged (BVH_TRIANGLE_FLAG_GLASS, see BvhMasked), where the ray queries' is a structure of its own; a hit on
// what doesn't move names its instance either way (BvhHit::modelInstance), a moving BVH's none

// the nearest triangle a ray from origin along direction meets before tmax (or with anyHit any), of those filter accepts:
// what doesn't move, and with moving what moves too. a ray query's is never capped; a BVH's may be (BvhHit::capped)
BvhHit BvhTraceScene<Filter : IBvhFilter>(float3 origin, float3 direction, float tmax, bool anyHit, bool moving, Filter filter)
{
	return BvhTraceInstances(
		origin, direction, tmax, anyHit, moving ? (BVH_RAY_QUERY_MASK_STATIC | BVH_RAY_QUERY_MASK_MOVING) : BVH_RAY_QUERY_MASK_STATIC, filter);
}

// the nearest transmissive triangle (that moves or not) a ray meets before tmax, which only the paths through the glass
// query (see GlassPath; BVH_RAY_QUERY_MASK_GLASS)
BvhHit BvhTraceGlass(float3 origin, float3 direction, float tmax)
{
	return BvhTraceInstances(origin, direction, tmax, false, BVH_RAY_QUERY_MASK_GLASS, BvhOpaque());
}

// the nearest of BvhTraceScene's (what moves too) and BvhTraceGlass's in one query, and whether it is glass: the paths
// through the glass (see GlassPath) took one of each for every surface they crossed (DragonAttenuation's transmissive
// phase 1.31 ms rather than 1.15)
BvhHit BvhTraceSceneOrGlass<Filter : IBvhFilter>(float3 origin, float3 direction, float tmax, Filter filter, out bool glass)
{
	uint instance;
	BvhHit hit = BvhTraceInstances(
		origin, direction, tmax, false, BVH_RAY_QUERY_MASK_STATIC | BVH_RAY_QUERY_MASK_MOVING | BVH_RAY_QUERY_MASK_GLASS, filter, instance);
	glass = instance == BVH_RAY_QUERY_INSTANCE_GLASS || instance == BVH_RAY_QUERY_INSTANCE_MOVING_GLASS;
	return hit;
}

// BvhTraceScene's, of the instances of mask
BvhHit BvhTraceInstances<Filter : IBvhFilter>(float3 origin, float3 direction, float tmax, bool anyHit, uint mask, Filter filter)
{
	uint instance;
	return BvhTraceInstances(origin, direction, tmax, anyHit, mask, filter, instance);
}

#if !BVH_RAY_QUERY
// a ray's filter through the BVHs, which hold the glass among the rest (BVH_TRIANGLE_FLAG_GLASS, see gfx/bvh.h): a
// glass triangle stops the rays of BVH_RAY_QUERY_MASK_GLASS alone (whole: its alpha mask isn't tested, as the ray
// queries' glass structure has it), any other those its BVH's mask bit is in (opaque), where the ray's own filter
// accepts it
struct BvhMasked<Filter : IBvhFilter> : IBvhFilter
{
	Filter filter;
	bool glass;
	bool opaque;

	bool Accept(uint flags, uint firstIndex, float2 barycentrics)
	{
		if ((flags & BVH_TRIANGLE_FLAG_GLASS) != 0u)
			return glass;
		return opaque && filter.Accept(flags, firstIndex, barycentrics);
	}
}

// and the top level instance it met (~0u where nothing; as the ray queries' structures name them: 0 what doesn't move,
// BVH_RAY_QUERY_INSTANCE_MOVING what moves, BVH_RAY_QUERY_INSTANCE_GLASS and _MOVING_GLASS the glass), through the BVHs:
// the static one, then the moving one where the model has one (TRACED_MOVING_BVH) for what is nearer, the glass in each
// for the rays of BVH_RAY_QUERY_MASK_GLASS (so the nearest of the scene and the glass is one traversal of each)
BvhHit BvhTraceInstances<Filter : IBvhFilter>(float3 origin, float3 direction, float tmax, bool anyHit, uint mask, Filter filter, out uint instance)
{
	instance = ~0u;
	BvhHit hit = BvhNothing(tmax);
	BvhMasked<Filter> masked;
	masked.filter = filter;
	masked.glass = (mask & BVH_RAY_QUERY_MASK_GLASS) != 0u;
	if ((mask & (BVH_RAY_QUERY_MASK_STATIC | BVH_RAY_QUERY_MASK_GLASS)) != 0u)
	{
		masked.opaque = (mask & BVH_RAY_QUERY_MASK_STATIC) != 0u;
		hit = BvhTraverse(gBvhNodes, gBvhTriangles, origin, direction, tmax, anyHit, masked);
		if (BvhHitSomething(hit))
			instance = (hit.flags & BVH_TRIANGLE_FLAG_GLASS) != 0u ? BVH_RAY_QUERY_INSTANCE_GLASS : 0u;
	}
	if ((mask & (BVH_RAY_QUERY_MASK_MOVING | BVH_RAY_QUERY_MASK_GLASS)) != 0u && (gPushConstants.traced & TRACED_MOVING_BVH) != 0u &&
		!(anyHit && BvhHitSomething(hit)))
	{
		uint frameIndex = gPushConstants.frameIndex;
		masked.opaque = (mask & BVH_RAY_QUERY_MASK_MOVING) != 0u;
		BvhHit movingHit = BvhTraverse2(gBvhMovingNodes[frameIndex], gBvhMovingTriangles[frameIndex], origin, direction, hit.t, anyHit, masked);
		bool capped = hit.capped || movingHit.capped;
		if (BvhHitSomething(movingHit))
		{
			hit = movingHit;
			hit.moves = true;
			instance = (hit.flags & BVH_TRIANGLE_FLAG_GLASS) != 0u ? BVH_RAY_QUERY_INSTANCE_MOVING_GLASS : BVH_RAY_QUERY_INSTANCE_MOVING;
		}
		hit.capped = capped;
	}
	return hit;
}
#else
// and the top level instance it met (~0u where nothing)
BvhHit BvhTraceInstances<Filter : IBvhFilter>(float3 origin, float3 direction, float tmax, bool anyHit, uint mask, Filter filter, out uint instance)
{
	instance = ~0u;
	bool moving = (mask & BVH_RAY_QUERY_MASK_MOVING) != 0u;
#if BVH_RAY_QUERY_WATCH
	uint watch = RayQueryWatchBegin(origin, direction, tmax, anyHit, moving);
	uint candidates = 0u;
#endif
	RayDesc ray;
	ray.Origin = origin;
	ray.TMin = 0.0;
	ray.Direction = direction;
	ray.TMax = tmax;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(gBvhScene[gPushConstants.frameIndex], anyHit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE, mask, ray);
#if BVH_RAY_QUERY_WATCH
	RayQueryWatchStage(watch, RAY_QUERY_WATCH_STAGE_TRACED, 0u);
#endif
	// the candidates are the alpha masked triangles (their geometry isn't opaque): hit where the filter accepts them
	while (query.Proceed())
	{
#if BVH_RAY_QUERY_WATCH
		if ((candidates++ & 15u) == 0u)
			RayQueryWatchStage(watch, RAY_QUERY_WATCH_STAGE_CANDIDATES, candidates);
#endif
		uint4 geometry = BvhRayQueryGeometry(query.CandidateInstanceID(), query.CandidateGeometryIndex());
		if (filter.Accept(geometry.x, geometry.y + query.CandidatePrimitiveIndex() * 3u, query.CandidateTriangleBarycentrics()))
			query.CommitNonOpaqueTriangleHit();
	}
#if BVH_RAY_QUERY_WATCH
	RayQueryWatchEnd(watch, candidates);
#endif
	if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
		return BvhNothing(tmax);
	instance = query.CommittedInstanceIndex();
	uint4 geometry = BvhRayQueryGeometry(query.CommittedInstanceID(), query.CommittedGeometryIndex());
	uint primitive = query.CommittedPrimitiveIndex();
	float3 corners[3];
	BvhRayQueryCorners(query.CommittedInstanceIndex(), geometry, primitive, corners);
	BvhHit hit = BvhNothing(tmax);
	hit.t = query.CommittedRayT();
	hit.barycentrics = query.CommittedTriangleBarycentrics();
	hit.primitive = primitive;
	hit.flags = geometry.x;
	hit.firstIndex = geometry.y + primitive * 3u;
	hit.moves = BvhRayQueryMoving(instance);
	if (!hit.moves)
		hit.modelInstance = geometry.z;
	hit.normalInstance = geometry.w;
	float3 normal = normalize(cross(corners[1] - corners[0], corners[2] - corners[0]));
	hit.frontFace = dot(normal, direction) < 0.0;
	hit.normal = hit.frontFace ? normal : -normal;
	return hit;
}

#if BVH_RAY_QUERY_WATCH
// diagnostic (SPEEDO_RAY_QUERY_REPLAY, see gfx/rayquerywatch.h): rays a crash report's watch had in flight, traced again as
// they were (a thread per ray along x, per repetition along y), the watch counting them, and those that hit something
// (so that the queries' results are used). gRayQueryReplay: the count, then per ray its origin and tmax, then its direction
// and flags (bit 0 any hit, 1 moving)
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeRayQueryReplay(uint3 dispatchId: SV_DispatchThreadID)
{
	uint count = asuint(gRayQueryReplay[0].x);
	if (dispatchId.x >= count)
		return;
	float4 originTmax = gRayQueryReplay[1u + dispatchId.x * 2u];
	float4 directionFlags = gRayQueryReplay[2u + dispatchId.x * 2u];
	uint flags = asuint(directionFlags.w);
	BvhHit hit = BvhTraceScene(originTmax.xyz, directionFlags.xyz, originTmax.w, (flags & 1u) != 0u, (flags & 2u) != 0u, BvhOpaque());
	if (hit.t < originTmax.w)
		InterlockedAdd(gRayQueryWatch[RAY_QUERY_WATCH_REPLAY_HITS], 1u);
}
#endif
#endif
