// ray queries against BVHs of triangles (see gfx/bvh.h), traversed as tinybvh's OpenCL kernels do: its compressed wide
// BVH (CWBVH, Ylitie et al. 2017; kernels/traverse_cwbvh.cl, after AlanWBFT's CUDA code) with its compressed triangles
// (CWBVH_COMPRESSED_TRIS, which tiny_bvh.h always defines: Baldwin and Weber's transform), what doesn't move; and its
// binary one in Aila and Laine's layout (BVH_GPU, kernels/traverse_bvh2.cl), what moves, refit each frame (encoding a
// CWBVH again took BrainStem 3 ms a frame, the binary layout's copy a tenth of that). the indirect lighting's rays use
// them (see GiData::trace). a part of shaders.slang, which includes it after capi.h

// the node groups still to visit (a node's children each): a wide tree's depth stays far below it; and the nodes still to
// visit of a binary tree (tinybvh's kernel's)
#ifndef BVH_STACK_SIZE
#define BVH_STACK_SIZE 48
#endif
#ifndef BVH2_STACK_SIZE
#define BVH2_STACK_SIZE 128
#endif
// the most nodes a ray visits (BvhHit::capped past it): a mesh whose triangles overlap everything (geodesic.zip's duals)
// has every ray visit most of its tree, and a dispatch of such rays ran past Metal's command queue timeout
// (VK_ERROR_DEVICE_LOST). a ray of the test scenes visits tens to a few hundred
#define BVH_MAX_STEP_COUNT 4096u
#define BVH2_MAX_STEP_COUNT 16384u

// what a ray met: the distance along it (in its direction's lengths; the ray's end where it met nothing), where on the
// triangle (barycentrics: the weights of its second and third corners), its index among the triangles built (~0 if
// none), its BVH_TRIANGLE_FLAG_* bits and first index (see gfx/bvh.h), its unit normal toward the side the ray came from,
// and whether that is its front (counterclockwise) side
struct BvhHit
{
	float t;
	float2 barycentrics;
	uint primitive;
	uint flags;
	uint firstIndex;
	float3 normal;
	bool frontFace;
	bool capped; // stopped at BVH_MAX_STEP_COUNT: the nearest of what it met by then
};

bool BvhHitSomething(BvhHit hit)
{
	return hit.primitive != ~0u;
}

BvhHit BvhNothing(float tmax)
{
	BvhHit hit;
	hit.t = tmax;
	hit.barycentrics = 0.0;
	hit.primitive = ~0u;
	hit.flags = 0;
	hit.firstIndex = 0;
	hit.normal = 0.0;
	hit.frontFace = false;
	hit.capped = false;
	return hit;
}

// whether a ray passes a triangle it hit (an alpha masked one, where its texture is cut out): asked of each hit nearer
// than the nearest so far (for any hit, until it accepts one)
interface IBvhFilter
{
	bool Accept(uint flags, uint firstIndex, float2 barycentrics);
}

// every triangle stops a ray
struct BvhOpaque : IBvhFilter
{
	bool Accept(uint flags, uint firstIndex, float2 barycentrics)
	{
		return true;
	}
}

// what the nearest hit says beyond its distance and barycentrics, read from its triangle (first float4 triangle, ~0 if
// none) once the traversal is done: of a CWBVH's (see BvhTraverse), and of a binary one's (see BvhTraverse2)
BvhHit BvhFinish(BvhHit hit, StructuredBuffer<float4> triangles, uint triangle, float3 direction)
{
	if (triangle == ~0u)
		return hit;
	float4 t2 = triangles[triangle + 2];
	float4 last = triangles[triangle + 3];
	hit.primitive = asuint(last.w);
	hit.flags = asuint(last.y);
	hit.firstIndex = asuint(last.z);
	// the transform's third row is the normal divided by its largest component, whose sign gfx/bvh.h keeps in the last
	// float4's x
	float facing = dot(t2.xyz, direction);
	hit.normal = normalize(t2.xyz) * (facing > 0.0 ? -1.0 : 1.0);
	hit.frontFace = facing * last.x < 0.0;
	return hit;
}

BvhHit BvhFinish2(BvhHit hit, StructuredBuffer<float4> triangles, uint triangle, float3 direction)
{
	if (triangle == ~0u)
		return hit;
	float4 corner = triangles[triangle];
	float4 edge1 = triangles[triangle + 1];
	float4 edge2 = triangles[triangle + 2];
	hit.primitive = asuint(corner.w);
	hit.flags = asuint(edge1.w);
	hit.firstIndex = asuint(edge2.w);
	float3 normal = normalize(cross(edge1.xyz, edge2.xyz));
	hit.frontFace = dot(normal, direction) < 0.0;
	hit.normal = hit.frontFace ? normal : -normal;
	return hit;
}

// a node's 4 quantized coordinates of a bound (bytes), as floats in [1, 2): the byte in the mantissa's top bits
float4 BvhUnpackQ8x4(uint w)
{
	const uint mantissa = 0x007F8000u;
	const uint one = 0x3F800000u;
	return asfloat((uint4(w << 15, w << 7, w >> 1, w >> 9) & mantissa) | one);
}

// which of 4 of a node's children the ray overlaps between 0 and tmax: their bits of the node's hit mask (child nodes in
// the top byte, triangles in the others, in the order the ray's octant visits them). meta holds the children's meta
// bytes, and lo and hi their bounds along each axis, nearest first along the ray (swapped by its direction's signs)
uint BvhChildHits(
	uint meta, uint octant, float4 loX, float4 hiX, float4 loY, float4 hiY, float4 loZ, float4 hiZ, float3 scale, float3 offset, float tmax)
{
	uint inner = (meta & (meta << 1)) & 0x10101010u;
	uint innerMask = (inner << 4) - (inner >> 4);
	uint bitIndex = (meta ^ (octant & innerMask)) & 0x1F1F1F1Fu;
	uint childBits = (meta >> 5) & 0x07070707u;
	float4 enter = max(max(max(mad(loX, scale.x, offset.x), mad(loY, scale.y, offset.y)), mad(loZ, scale.z, offset.z)), 0.0);
	float4 leave = min(min(min(mad(hiX, scale.x, offset.x), mad(hiY, scale.y, offset.y)), mad(hiZ, scale.z, offset.z)), tmax);
	uint hits = 0;
	[unroll]
	for (uint childIt = 0; childIt < 4; childIt++)
		if (enter[childIt] <= leave[childIt])
			hits |= ((childBits >> (childIt * 8u)) & 255u) << ((bitIndex >> (childIt * 8u)) & 31u);
	return hits;
}

// the nearest triangle a ray from origin along direction meets before tmax (anyHit false), or any one (anyHit true: a
// shadow ray, which needs no nearest), of those filter accepts, in a CWBVH's nodes (BVH_NODE_FLOAT4_COUNT float4 each)
// and triangles (BVH_TRIANGLE_FLOAT4_COUNT each; see gfx/bvh.h). barycentrics: the weights of its second and third
// corners
BvhHit BvhTraverse<Filter : IBvhFilter>(
	StructuredBuffer<float4> nodes,
	StructuredBuffer<float4> triangles,
	float3 origin,
	float3 direction,
	float tmax,
	bool anyHit,
	Filter filter)
{
	BvhHit hit = BvhNothing(tmax);
	uint hitTriangle = ~0u; // the nearest so far's first float4 (the rest of what the hit says is read once, at the end)
	// a direction's zero component as a huge reciprocal of its sign (tinybvh's tinybvh_safercp)
	const float kFar = asfloat(0x71800000u); // 2^100
	float3 inverse = select(abs(direction) > 1e-30, 1.0 / direction, select(direction < 0.0, -kFar, kFar));
	uint octant = (7u - ((direction.x < 0.0 ? 4u : 0u) | (direction.y < 0.0 ? 2u : 0u) | (direction.z < 0.0 ? 1u : 0u))) * 0x01010101u;
	float3 inverseScaled = inverse * 256.0; // the quantized bounds' scale

	// a node group still to visit is its parent node's index and its hit bits not yet visited (the top byte): its first
	// child and inner mask are the parent's (as tinybvh's dev kernel keeps it, a uint an entry rather than two). past the
	// stack's size, groups are dropped, not written past it
	uint stack[BVH_STACK_SIZE];
	uint stackSize = 0;
	// the node group (its first node, and its hit bits in the top byte above the inner mask) and triangle group (its
	// first triangle, and its hit bits in the low 24) being visited: the root first
	uint2 nodeGroup = uint2(0, 0x80000000u);
	uint2 triangleGroup = uint2(0, 0);
	uint parent = 0;
	for (uint step = 0;; step++)
	{
		if (step == BVH_MAX_STEP_COUNT)
		{
			hit.capped = true;
			break;
		}
		{
			// the group's next child node, in the order the ray's octant visits them
			uint childBit = firstbithigh(nodeGroup.y);
			uint mask = nodeGroup.y;
			uint childBase = nodeGroup.x;
			nodeGroup.y &= ~(1u << childBit);
			if (nodeGroup.y > 0x00FFFFFFu && stackSize < BVH_STACK_SIZE)
				stack[stackSize++] = (parent << 8) | (nodeGroup.y >> 24);
			uint slot = (childBit - 24u) ^ (octant & 255u);
			uint relative = countbits(mask & ~(0xFFFFFFFFu << slot));
			parent = childBase + relative;
			uint node = parent * BVH_NODE_FLOAT4_COUNT;
			float4 n0 = nodes[node + 0];
			float4 n1 = nodes[node + 1];
			float4 n2 = nodes[node + 2];
			float4 n3 = nodes[node + 3];
			float4 n4 = nodes[node + 4];
			// n0's w packs the three quantization exponents (already IEEE-754 exponent fields) and the inner node mask
			uint exponents = asuint(n0.w);
			nodeGroup.x = asuint(n1.x);
			triangleGroup = uint2(asuint(n1.y), 0);
			float3 scale = float3(
				asfloat((exponents & 255u) << 23) * inverseScaled.x,
				asfloat(((exponents >> 8) & 255u) << 23) * inverseScaled.y,
				asfloat(((exponents >> 16) & 255u) << 23) * inverseScaled.z);
			float3 offset = (n0.xyz - origin) * inverse - scale;
			uint hits = BvhChildHits(
				asuint(n1.z), octant,
				BvhUnpackQ8x4(asuint(inverse.x < 0.0 ? n3.z : n2.x)), BvhUnpackQ8x4(asuint(inverse.x < 0.0 ? n2.x : n3.z)),
				BvhUnpackQ8x4(asuint(inverse.y < 0.0 ? n4.x : n2.z)), BvhUnpackQ8x4(asuint(inverse.y < 0.0 ? n2.z : n4.x)),
				BvhUnpackQ8x4(asuint(inverse.z < 0.0 ? n4.z : n3.x)), BvhUnpackQ8x4(asuint(inverse.z < 0.0 ? n3.x : n4.z)),
				scale, offset, hit.t);
			hits |= BvhChildHits(
				asuint(n1.w), octant,
				BvhUnpackQ8x4(asuint(inverse.x < 0.0 ? n3.w : n2.y)), BvhUnpackQ8x4(asuint(inverse.x < 0.0 ? n2.y : n3.w)),
				BvhUnpackQ8x4(asuint(inverse.y < 0.0 ? n4.y : n2.w)), BvhUnpackQ8x4(asuint(inverse.y < 0.0 ? n2.w : n4.y)),
				BvhUnpackQ8x4(asuint(inverse.z < 0.0 ? n4.w : n3.y)), BvhUnpackQ8x4(asuint(inverse.z < 0.0 ? n3.y : n4.w)),
				scale, offset, hit.t);
			nodeGroup.y = (hits & 0xFF000000u) | (exponents >> 24);
			triangleGroup.y = hits & 0x00FFFFFFu;
		}
		// the group's triangles (Baldwin and Weber: the plane's distance first, then the barycentrics where it is met;
		// without continue, see GiMarch)
		while (triangleGroup.y != 0u)
		{
			uint index = firstbithigh(triangleGroup.y);
			uint triangle = triangleGroup.x + index * BVH_TRIANGLE_FLOAT4_COUNT;
			triangleGroup.y -= 1u << index;
			float4 t2 = triangles[triangle + 2];
			float distance = -(dot(t2.xyz, origin) + t2.w) / dot(t2.xyz, direction);
			if (distance > 0.0 && distance < hit.t)
			{
				float4 t0 = triangles[triangle + 0];
				float4 t1 = triangles[triangle + 1];
				float3 position = origin + distance * direction;
				float u = dot(t0.xyz, position) + t0.w;
				float v = dot(t1.xyz, position) + t1.w;
				if (u >= 0.0 && v >= 0.0 && u + v < 1.0)
				{
					float4 last = triangles[triangle + 3];
					if (filter.Accept(asuint(last.y), asuint(last.z), float2(u, v)))
					{
						hit.t = distance;
						hit.barycentrics = float2(u, v);
						hitTriangle = triangle;
						if (anyHit)
							return BvhFinish(hit, triangles, hitTriangle, direction);
					}
				}
			}
		}
		if (nodeGroup.y <= 0x00FFFFFFu)
		{
			if (stackSize == 0u)
				break;
			uint entry = stack[--stackSize];
			parent = entry >> 8;
			uint parentNode = parent * BVH_NODE_FLOAT4_COUNT;
			nodeGroup = uint2(asuint(nodes[parentNode + 1].x), ((entry & 255u) << 24) | (asuint(nodes[parentNode].w) >> 24));
		}
	}
	return BvhFinish(hit, triangles, hitTriangle, direction);
}

// the nearest triangle (or any, with anyHit) a ray meets before tmax, of those filter accepts, in a binary BVH in Aila and
// Laine's layout (tinybvh's BVH_GPU): nodes of BVH2_NODE_FLOAT4_COUNT float4 (the children's bounds, their references in
// the w's: a leaf's with bit 31, its triangle count in bits 24 to 30 and its first triangle below), triangles of
// BVH2_TRIANGLE_FLOAT4_COUNT (the first corner and the two edges from it, Moller and Trumbore's test; w: the index among
// the triangles built, the flags and the first index, see gfx/bvh.h)
BvhHit BvhTraverse2<Filter : IBvhFilter>(
	StructuredBuffer<float4> nodes, StructuredBuffer<float4> triangles, float3 origin, float3 direction, float tmax, bool anyHit, Filter filter)
{
	BvhHit hit = BvhNothing(tmax);
	uint hitTriangle = ~0u; // see BvhTraverse
	const float kFar = asfloat(0x71800000u); // 2^100, see BvhTraverse
	float3 inverse = select(abs(direction) > 1e-30, 1.0 / direction, select(direction < 0.0, -kFar, kFar));
	float3 offset = origin * -inverse;
	uint stack[BVH2_STACK_SIZE];
	uint stackSize = 0;
	uint node = 0;
	for (uint step = 0;; step++)
	{
		if (step == BVH2_MAX_STEP_COUNT)
		{
			hit.capped = true;
			break;
		}
		if ((node & 0x80000000u) != 0u)
		{
			uint count = (node >> 24) & 127u;
			uint first = (node & 0x00FFFFFFu) * BVH2_TRIANGLE_FLOAT4_COUNT;
			for (uint triangleIt = 0; triangleIt < count; triangleIt++)
			{
				uint triangle = first + triangleIt * BVH2_TRIANGLE_FLOAT4_COUNT;
				float4 corner = triangles[triangle];
				float4 edge1 = triangles[triangle + 1];
				float4 edge2 = triangles[triangle + 2];
				float3 h = cross(direction, edge2.xyz);
				float a = dot(edge1.xyz, h);
				float f = 1.0 / a;
				float3 s = origin - corner.xyz;
				float u = f * dot(s, h);
				float3 q = cross(s, edge1.xyz);
				float v = f * dot(direction, q);
				float distance = f * dot(edge2.xyz, q);
				if (u >= 0.0 && v >= 0.0 && u + v <= 1.0 && distance > 0.0 && distance < hit.t &&
					filter.Accept(asuint(edge1.w), asuint(edge2.w), float2(u, v)))
				{
					hit.t = distance;
					hit.barycentrics = float2(u, v);
					hitTriangle = triangle;
					if (anyHit)
						return BvhFinish2(hit, triangles, hitTriangle, direction);
				}
			}
			if (stackSize == 0u)
				break;
			node = stack[--stackSize];
		}
		else
		{
			uint at = node * BVH2_NODE_FLOAT4_COUNT;
			float4 leftLow = nodes[at];
			float4 leftHigh = nodes[at + 1];
			float4 rightLow = nodes[at + 2];
			float4 rightHigh = nodes[at + 3];
			float3 leftA = mad(leftLow.xyz, inverse, offset);
			float3 leftB = mad(leftHigh.xyz, inverse, offset);
			float3 rightA = mad(rightLow.xyz, inverse, offset);
			float3 rightB = mad(rightHigh.xyz, inverse, offset);
			float3 leftNear = min(leftA, leftB);
			float3 leftFar = max(leftA, leftB);
			float3 rightNear = min(rightA, rightB);
			float3 rightFar = max(rightA, rightB);
			float leftEnter = max(max(max(leftNear.x, leftNear.y), leftNear.z), 0.0);
			float rightEnter = max(max(max(rightNear.x, rightNear.y), rightNear.z), 0.0);
			bool leftHit = leftEnter <= min(min(min(leftFar.x, leftFar.y), leftFar.z), hit.t);
			bool rightHit = rightEnter <= min(min(min(rightFar.x, rightFar.y), rightFar.z), hit.t);
			uint left = asuint(leftLow.w);
			uint right = asuint(leftHigh.w);
			if (leftHit && rightHit)
			{
				// the nearer first, the other for later (past the stack's size, dropped rather than written past it)
				bool rightFirst = rightEnter < leftEnter;
				if (stackSize < BVH2_STACK_SIZE)
					stack[stackSize++] = rightFirst ? left : right;
				node = rightFirst ? right : left;
			}
			else if (leftHit)
				node = left;
			else if (rightHit)
				node = right;
			else
			{
				if (stackSize == 0u)
					break;
				node = stack[--stackSize];
			}
		}
	}
	return BvhFinish2(hit, triangles, hitTriangle, direction);
}

#if BVH_RAY_QUERY
// a ray query's triangle (see gfx/accelerationstructures.h): its first float4, in what doesn't move (gBvhTriangles) or what
// moves (gBvhMovingTriangles[frame]) by its instance, from its geometry's first (the opaque ones from 0, the alpha masked
// from the instance's custom index) and its index in its geometry
uint BvhRayQueryTriangle(uint customIndex, uint geometry, uint primitive)
{
	return ((geometry == 0u ? 0u : customIndex) + primitive) * BVH_RAY_QUERY_TRIANGLE_FLOAT4_COUNT;
}

float4 BvhRayQueryRead(uint instance, uint at)
{
	if (instance == BVH_RAY_QUERY_INSTANCE_MOVING)
		return gBvhMovingTriangles[gPushConstants.frameIndex][at];
	return gBvhTriangles[at];
}

// the nearest triangle a ray from origin along direction meets before tmax (or with anyHit any), of those filter accepts,
// as the device's ray queries find it in gBvhScene[frame] (where they trace instead of BvhTraverse and BvhTraverse2): what
// doesn't move, and with moving what moves too. what it says is as theirs (never capped)
BvhHit BvhTraceScene<Filter : IBvhFilter>(float3 origin, float3 direction, float tmax, bool anyHit, bool moving, Filter filter)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.TMin = 0.0;
	ray.Direction = direction;
	ray.TMax = tmax;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(
		gBvhScene[gPushConstants.frameIndex],
		anyHit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE,
		moving ? (BVH_RAY_QUERY_MASK_STATIC | BVH_RAY_QUERY_MASK_MOVING) : BVH_RAY_QUERY_MASK_STATIC,
		ray);
	// the candidates are the alpha masked triangles (their geometry isn't opaque): hit where the filter accepts them
	while (query.Proceed())
	{
		uint instance = query.CandidateInstanceIndex();
		uint at = BvhRayQueryTriangle(query.CandidateInstanceID(), query.CandidateGeometryIndex(), query.CandidatePrimitiveIndex());
		if (filter.Accept(asuint(BvhRayQueryRead(instance, at).w), asuint(BvhRayQueryRead(instance, at + 1u).w), query.CandidateTriangleBarycentrics()))
			query.CommitNonOpaqueTriangleHit();
	}
	if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
		return BvhNothing(tmax);
	uint instance = query.CommittedInstanceIndex();
	uint at = BvhRayQueryTriangle(query.CommittedInstanceID(), query.CommittedGeometryIndex(), query.CommittedPrimitiveIndex());
	float4 corner0 = BvhRayQueryRead(instance, at);
	float4 corner1 = BvhRayQueryRead(instance, at + 1u);
	float4 corner2 = BvhRayQueryRead(instance, at + 2u);
	BvhHit hit = BvhNothing(tmax);
	hit.t = query.CommittedRayT();
	hit.barycentrics = query.CommittedTriangleBarycentrics();
	hit.primitive = at / BVH_RAY_QUERY_TRIANGLE_FLOAT4_COUNT;
	hit.flags = asuint(corner0.w);
	hit.firstIndex = asuint(corner1.w);
	float3 normal = normalize(cross(corner1.xyz - corner0.xyz, corner2.xyz - corner0.xyz));
	hit.frontFace = dot(normal, direction) < 0.0;
	hit.normal = hit.frontFace ? normal : -normal;
	return hit;
}
#endif
