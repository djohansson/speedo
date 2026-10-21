// bvh.slang's traversal on the cpu, as slang's c++ target compiles it (see src/tools/bvhkernelsbridge.h): a thread per ray

#include "capi.h"
#include "bvh.slang"

StructuredBuffer<float4> gNodes;
StructuredBuffer<float4> gTriangles;
// per ray: its origin and end (w), its direction and whether any hit will do (w 1)
StructuredBuffer<float4> gRays;
// per ray: the distance, barycentrics and primitive (float bits), then whether it met the triangle's front (x 1) and
// whether its traversal hit its step cap (y 1)
RWStructuredBuffer<float4> gHits;

void Write(uint ray, BvhHit hit)
{
	gHits[ray * 2] = float4(hit.t, hit.barycentrics, asfloat(hit.primitive));
	gHits[ray * 2 + 1] = float4(BvhHitSomething(hit) && hit.frontFace ? 1.0 : 0.0, hit.capped ? 1.0 : 0.0, 0.0, 0.0);
}

// through a CWBVH (BvhTraverse), and through a binary BVH in the moving BVH's layout (BvhTraverse2)
[shader("compute")]
[numthreads(64, 1, 1)]
void BvhTrace(uint3 dispatchId: SV_DispatchThreadID)
{
	float4 origin = gRays[dispatchId.x * 2];
	float4 direction = gRays[dispatchId.x * 2 + 1];
	Write(dispatchId.x, BvhTraverse(gNodes, gTriangles, origin.xyz, direction.xyz, origin.w, direction.w != 0.0, BvhOpaque()));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void BvhTrace2(uint3 dispatchId: SV_DispatchThreadID)
{
	float4 origin = gRays[dispatchId.x * 2];
	float4 direction = gRays[dispatchId.x * 2 + 1];
	Write(dispatchId.x, BvhTraverse2(gNodes, gTriangles, origin.xyz, direction.xyz, origin.w, direction.w != 0.0, BvhOpaque()));
}
