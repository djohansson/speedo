// emissive surfaces as lights (see EmitterData, gfx/emitters.h): the main pass's opaque pixels and the indirect
// lighting's voxels take the light of the model's emissive triangles by sampling points of them, shadowed by a ray
// through the BVHs. an emitter met by the probes', the cache's or the near field's rays was light by chance: a small
// bright one, a lantern's bulb, lit a wall in the few places some ray met it (blotches that followed the camera, and
// fireflies), and spread over the voxels it was in it lit as if it were a voxel big (Bistro's square yellow, a hundred
// times the bulbs' light); so those rays take only the light what they meet reflects (GiHitRadiance), and what the
// emitters give arrives through these. per traced pixel (every EmitterData::stride-th pixel each way), resampled
// importance sampling (RIS, Talbot et al. 2005) over candidates picked down a tree of the emitters by how much each part
// of it may light the pixel (EmitterPick, a light BVH; Emitter Sample, then Emitter Shade, which traces the chosen
// point's shadow ray; the reservoirs can be reused from the last frame's and the pixels around, as ReSTIR does, Bitterli
// et al. 2020, but aren't by default: see ComputeEmitterSample), filtered over the traced pixels (Emitter X and Y),
// then upsampled to every pixel and accumulated over frames there (Emitter Temporal): the irradiance over pi the main
// pass's diffuse takes (EmitterIrradiance). a part of shaders.slang, which includes it after ginear.slang

// the emitters' textures are read at a coarse level: a point's light stands for the area around it
static const float kEmitterTextureLevel = 3.0;

// the most traced pixels around a traced pixel its reservoir is merged with (EmitterData::spatialCount)
static const uint kEmitterSpatialCountMost = 8u;

// whether emissive surfaces light the scene this frame (else the indirect lighting's rays take their emission)
bool EmittersLight()
{
	return gEmitterData[gPushConstants.frameIndex][0].count > 0u;
}

float EmitterLuminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// a pcg sequence of numbers in [0, 1)
struct EmitterRandom
{
	uint state;

	[mutating]
	float Next()
	{
		state = state * 747796405u + 2891336453u;
		uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		word = (word >> 22u) ^ word;
		return float(word >> 8u) * (1.0 / 16777216.0);
	}
}

EmitterRandom EmitterRandomAt(uint2 place, uint seed)
{
	EmitterRandom random;
	random.state = ShadowHash(place ^ uint2(seed * 0x9e3779b9u, seed * 0x85ebca6bu));
	return random;
}

// a point of an emitter triangle (by the barycentrics of its second and third corners): where it is, its normal (toward
// its front), the radiance it emits (its material's emission as the frame has it, times its texture's there), and its
// triangle's area (a point of it is picked uniformly)
struct EmitterPoint
{
	float3 position;
	float3 normal;
	float3 emission;
	float area;
	bool doubleSided;
};

EmitterPoint EmitterPointAt(uint triangle, float2 barycentrics)
{
	EmitterTriangle emitter = gEmitters[triangle];
	EmitterPoint point;
	point.position = emitter.corner.xyz + emitter.edge1.xyz * barycentrics.x + emitter.edge2.xyz * barycentrics.y;
	float3 across = cross(emitter.edge1.xyz, emitter.edge2.xyz);
	float twiceArea = max(length(across), 1e-30);
	point.normal = across / twiceArea;
	point.area = 0.5 * twiceArea;
	uint flags = asuint(emitter.edge2.w);
	point.doubleSided = (flags & EMITTER_FLAG_DOUBLE_SIDED) != 0u;
	MaterialData material = gMaterialData[asuint(emitter.corner.w)];
	point.emission = material.emissive.rgb;
	if (any(point.emission > 0.0) && (material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
	{
		uint first = asuint(emitter.edge1.w);
		// a mirroring instance's edges are swapped (see EMITTER_FLAG_MIRRORED): its barycentrics weigh the other corners
		float2 weights = (flags & EMITTER_FLAG_MIRRORED) != 0u ? barycentrics.yx : barycentrics;
		float4 texCoord01 = gVertexBuffer[gIndexBuffer[first]].texCoord01 * (1.0 - weights.x - weights.y) +
							gVertexBuffer[gIndexBuffer[first + 1]].texCoord01 * weights.x +
							gVertexBuffer[gIndexBuffer[first + 2]].texCoord01 * weights.y;
		TextureView view = gTextureViews[material.emissiveView];
		point.emission *= gTextures[view.textureId].SampleLevel(gSamplers[view.samplerId], ViewTexCoord(view, texCoord01), kEmitterTextureLevel).rgb;
	}
	return point;
}

// the cosine and sine of the angle between two, less the second, no less than 0 (pbrt's cosSubClamped, sinSubClamped)
float EmitterCosineLess(float sinA, float cosA, float sinB, float cosB)
{
	return cosA > cosB ? 1.0 : cosA * cosB + sinA * sinB;
}

float EmitterSineLess(float sinA, float cosA, float sinB, float cosB)
{
	return cosA > cosB ? 0.0 : sinA * cosB - cosA * sinB;
}

// how much a node of the tree may light a surface (at position, of normal, facing it: pbrt's LightBounds::Importance):
// its power over the distance squared to its box's center (no nearer than the box's bounding sphere), by the cosine of
// the least angle its triangles may face the surface at (between its cone and the directions to its box, the emission's
// cosine falloff, 0 past a quarter turn) and the least the surface may face it at. never 0 where something it holds
// lights the surface (the picks' densities must not be: see EmitterCandidates)
float EmitterImportance(EmitterNode node, float3 position, float3 normal)
{
	float3 center = (node.low.xyz + node.high.xyz) * 0.5;
	float3 offset = position - center;
	float distance2 = dot(offset, offset);
	float3 extent = (node.high.xyz - node.low.xyz) * 0.5;
	float radius2 = dot(extent, extent);
	float3 toSurface = offset * rsqrt(max(distance2, 1e-30));
	// the directions to the box from the surface: all of them inside its bounding sphere
	float cosBox = distance2 > radius2 ? sqrt(max(1.0 - radius2 / distance2, 0.0)) : -1.0;
	float sinBox = sqrt(max(1.0 - cosBox * cosBox, 0.0));
	// the least angle between the cone and the direction to the surface, then less the box's
	float cosCone = node.high.w;
	float sinCone = sqrt(max(1.0 - cosCone * cosCone, 0.0));
	float cosAxis = dot(node.axis.xyz, toSurface);
	if ((asuint(node.axis.w) & EMITTER_NODE_DOUBLE_SIDED) != 0u)
		cosAxis = abs(cosAxis);
	float sinAxis = sqrt(max(1.0 - cosAxis * cosAxis, 0.0));
	float cosOutside = EmitterCosineLess(sinAxis, cosAxis, sinCone, cosCone);
	float sinOutside = EmitterSineLess(sinAxis, cosAxis, sinCone, cosCone);
	float cosEmitted = EmitterCosineLess(sinOutside, cosOutside, sinBox, cosBox);
	if (cosEmitted <= 0.0)
		return 0.0;
	// and the surface's own: toward the box
	float cosIncident = -dot(normal, toSurface);
	float sinIncident = sqrt(max(1.0 - cosIncident * cosIncident, 0.0));
	float cosReceived = EmitterCosineLess(sinIncident, cosIncident, sinBox, cosBox);
	if (cosReceived <= 0.0)
		return 0.0;
	return node.low.w * cosEmitted * cosReceived / max(distance2, radius2);
}

// a triangle for a surface, down the tree from its root (gEmitterTree), each child by its importance's share of the two
// (EmitterImportance): the triangle (~0 where no node may light the surface), and the probability it was picked with
uint EmitterPick(inout EmitterRandom random, float3 position, float3 normal, out float probability)
{
	probability = 1.0;
	uint node = 0;
	for (uint depthIt = 0; depthIt < EMITTER_NODE_DEPTH_MOST; depthIt++)
	{
		uint bits = asuint(gEmitterTree[node].axis.w);
		if ((bits & EMITTER_NODE_LEAF) != 0u)
			return bits & EMITTER_NODE_INDEX_MASK;
		uint second = bits & EMITTER_NODE_INDEX_MASK;
		float first = EmitterImportance(gEmitterTree[node + 1u], position, normal);
		float other = EmitterImportance(gEmitterTree[second], position, normal);
		if (!(first + other > 0.0))
			break;
		float share = first / (first + other);
		if (random.Next() < share)
		{
			node = node + 1u;
			probability *= share;
		}
		else
		{
			node = second;
			probability *= 1.0 - share;
		}
	}
	probability = 0.0;
	return ~0u;
}

// a point of a triangle, uniformly by area
float2 EmitterBarycentrics(inout EmitterRandom random)
{
	float2 u = float2(random.Next(), random.Next());
	return u.x + u.y > 1.0 ? 1.0 - u : u;
}

// what an emitter point gives a surface (at position, of normal, facing it), unshadowed, per unit of its radiance: the
// cosines at both ends over the distance squared, which is taken no nearer than nearest (squared: a point beside the
// surface, a pixel or a voxel away, would be a firefly), and the unit direction to it and its distance
float EmitterGeometry(float3 position, float3 normal, EmitterPoint point, float nearest2, out float3 toPoint, out float distance)
{
	float3 offset = point.position - position;
	float distance2 = dot(offset, offset);
	distance = sqrt(distance2);
	toPoint = offset / max(distance, 1e-30);
	float cosine = dot(normal, toPoint);
	float emitted = -dot(point.normal, toPoint);
	if (point.doubleSided)
		emitted = abs(emitted);
	return max(cosine, 0.0) * max(emitted, 0.0) / max(distance2, nearest2);
}

// the resampling's target at a surface: the luminance of the irradiance an emitter point gives it, unshadowed
float EmitterTarget(float3 position, float3 normal, EmitterPoint point, float nearest2)
{
	float3 toPoint;
	float distance;
	return EmitterLuminance(point.emission) * EmitterGeometry(position, normal, point, nearest2, toPoint, distance);
}


// a reservoir (weighted reservoir sampling of emitter points): the point chosen (its triangle, ~0 for none, and
// barycentrics), its target where it is built (target), and the sum of the weights while it is built, its contribution
// weight once finished (W: the irradiance a point gives, times it, is an unbiased estimate of all of theirs), how many
// candidates it stands for (M), and for how many frames its point has been known to reach the pixel (0: not known, see
// ComputeEmitterShade)
struct EmitterReservoir
{
	uint triangle;
	float2 barycentrics;
	float target;
	float weight;
	float count;
	uint age;

	// a candidate, by its weight (its target over the density it was drawn with): whether it is chosen (for now)
	[mutating]
	bool Add(uint candidate, float2 at, float candidateTarget, float candidateWeight, inout EmitterRandom random)
	{
		weight += candidateWeight;
		if (!(candidateWeight > 0.0 && random.Next() * weight < candidateWeight))
			return false;
		triangle = candidate;
		barycentrics = at;
		target = candidateTarget;
		age = 0u;
		return true;
	}

	// a finished reservoir (of another pixel, or frame), by its point's target here, as the candidates it stands for:
	// whether its point is chosen (for now)
	[mutating]
	bool Merge(EmitterReservoir other, float otherTarget, inout EmitterRandom random)
	{
		count += other.count;
		return other.triangle != ~0u && Add(other.triangle, other.barycentrics, otherTarget, otherTarget * other.weight * other.count, random);
	}

	// its weight from the sum: over the count and the chosen point's target
	[mutating]
	void Finish()
	{
		weight = triangle != ~0u && target > 0.0 && count > 0.0 ? weight / (count * target) : 0.0;
		if (weight <= 0.0)
			triangle = ~0u;
	}
}

EmitterReservoir EmitterEmpty()
{
	EmitterReservoir reservoir;
	reservoir.triangle = ~0u;
	reservoir.barycentrics = 0.0;
	reservoir.target = 0.0;
	reservoir.weight = 0.0;
	reservoir.count = 0.0;
	reservoir.age = 0u;
	return reservoir;
}

// a finished reservoir in gEmitterReservoirs: the triangle, the barycentrics as unorm16 each, W, and M (whole: the
// candidates' counts are) and the age, 16 bits each
uint4 EmitterPack(EmitterReservoir reservoir)
{
	uint2 at = uint2(saturate(reservoir.barycentrics) * 65535.0 + 0.5);
	uint count = uint(min(reservoir.count, 65535.0) + 0.5);
	return uint4(reservoir.triangle, at.x | (at.y << 16u), asuint(reservoir.weight), count | (min(reservoir.age, 65535u) << 16u));
}

EmitterReservoir EmitterUnpack(uint4 packed)
{
	EmitterReservoir reservoir = EmitterEmpty();
	reservoir.triangle = packed.x;
	reservoir.barycentrics = float2(packed.y & 0xffffu, packed.y >> 16u) / 65535.0;
	reservoir.weight = asfloat(packed.z);
	reservoir.age = packed.w >> 16u;
	if (reservoir.triangle >= gEmitterData[gPushConstants.frameIndex][0].count || !(reservoir.weight > 0.0) || isinf(reservoir.weight))
		reservoir = EmitterEmpty();
	// the candidates it stands for, a point or none (a hidden one's, see ComputeEmitterShade)
	reservoir.count = float(packed.w & 0xffffu);
	return reservoir;
}

// a reservoir's point's target at a surface
float EmitterTargetOf(EmitterReservoir reservoir, float3 position, float3 normal, float nearest2)
{
	return reservoir.triangle == ~0u ? 0.0 : EmitterTarget(position, normal, EmitterPointAt(reservoir.triangle, reservoir.barycentrics), nearest2);
}

// a surface's candidates (count of them, picked down the tree, see EmitterPick, a point of each uniformly), resampled by
// their target: the reservoir of the one chosen, finished
EmitterReservoir EmitterCandidates(float3 position, float3 normal, uint count, float nearest2, inout EmitterRandom random)
{
	EmitterReservoir reservoir = EmitterEmpty();
	for (uint candidateIt = 0; candidateIt < count; candidateIt++)
	{
		float probability;
		uint triangle = EmitterPick(random, position, normal, probability);
		float2 barycentrics = EmitterBarycentrics(random);
		if (triangle == ~0u)
			continue;
		EmitterPoint point = EmitterPointAt(triangle, barycentrics);
		float target = EmitterTarget(position, normal, point, nearest2);
		// its density per area: the triangle's probability, uniformly over it
		reservoir.Add(triangle, barycentrics, target, target * point.area / max(probability, 1e-30), random);
	}
	reservoir.count = float(count);
	reservoir.Finish();
	return reservoir;
}

// the irradiance a finished reservoir's point gives a surface, shadowed: its ray through the BVHs (alpha masks cut out;
// what moves with moving), from off the surface by bias toward the point, ending short of it (the emitter's own
// triangle), unless it is known to reach the surface (!trace); and whether it does (visible)
float3 EmitterShade(EmitterReservoir reservoir, float3 position, float3 normal, float bias, float nearest2, bool moving, bool trace, out bool visible)
{
	visible = false;
	if (reservoir.triangle == ~0u)
		return 0.0;
	EmitterPoint point = EmitterPointAt(reservoir.triangle, reservoir.barycentrics);
	float3 toPoint;
	float distance;
	float geometry = EmitterGeometry(position, normal, point, nearest2, toPoint, distance);
	if (geometry <= 0.0)
		return 0.0;
	float3 origin = position + normal * bias;
	float3 direction = point.position - origin;
	float span = sqrt(dot(direction, direction));
	if (trace && ShadowOccluder(origin, direction / max(span, 1e-30), span * 0.999, moving) >= 0.0)
		return 0.0;
	visible = true;
	return point.emission * geometry * reservoir.weight;
}

// the light the emitters give a voxel (at position, of normal: from the front only, as the punctual lights'): count
// samples, each resampled from candidateCount candidates and shadowed by a ray as the voxels' punctual lights are
// (GiVisibility: through the BVHs, or the voxels, short of the emitter's own voxel), nearer than a voxel taken as a
// voxel away; their mean. seed: the voxel's, for the samples' numbers
float3 EmitterVoxelIrradiance(GiData gi, float3 position, float3 normal, bool moving, uint2 seed)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	float voxel = gi.volumeOrigin.w;
	float nearest2 = voxel * voxel;
	bool bvh = GiTracesBvh(gi);
	EmitterRandom random = EmitterRandomAt(seed, moving ? data.frame : 0u);
	float3 irradiance = 0.0;
	for (uint sampleIt = 0; sampleIt < data.voxelSampleCount; sampleIt++)
	{
		EmitterReservoir reservoir = EmitterCandidates(position, normal, data.voxelCandidateCount, nearest2, random);
		if (reservoir.triangle == ~0u)
			continue;
		EmitterPoint point = EmitterPointAt(reservoir.triangle, reservoir.barycentrics);
		float3 toPoint;
		float distance;
		float geometry = EmitterGeometry(position, normal, point, nearest2, toPoint, distance);
		float reach = bvh ? distance * 0.999 : distance - 1.5 * voxel;
		if (geometry <= 0.0 || reach <= 0.0)
			continue;
		irradiance += point.emission * geometry * reservoir.weight * GiVisibility(gi, position, toPoint, reach, moving);
	}
	return data.voxelSampleCount > 0u ? irradiance / float(data.voxelSampleCount) : 0.0;
}

// the traced pixels: every EmitterData::stride-th pixel each way of the frame (the main depth's), each a texel of the
// emitters' images (SHADER_TYPES_EMITTER_RAW_TEXTURE on)
uint2 EmitterTracedExtent()
{
	uint2 extent;
	gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].GetDimensions(extent.x, extent.y);
	uint stride = gEmitterData[gPushConstants.frameIndex][0].stride;
	return (extent + stride - 1u) / stride;
}

// a unit normal in a float, exactly (an octahedral map's 11 bits each way, x + 2048 y): the traced pixels' surfaces'
float EmitterPackNormal(float3 normal)
{
	float2 folded = normal.xy / (abs(normal.x) + abs(normal.y) + abs(normal.z));
	if (normal.z < 0.0)
		folded = (1.0 - abs(folded.yx)) * select(folded >= 0.0, float2(1.0), float2(-1.0));
	uint2 quantized = uint2(round(saturate(folded * 0.5 + 0.5) * 2047.0));
	return float(quantized.x + quantized.y * 2048u);
}

float3 EmitterUnpackNormal(float packed)
{
	uint value = uint(packed);
	float2 folded = float2(value & 2047u, value >> 11u) / 2047.0 * 2.0 - 1.0;
	float3 normal = float3(folded, 1.0 - abs(folded.x) - abs(folded.y));
	if (normal.z < 0.0)
		normal.xy = (1.0 - abs(normal.yx)) * select(normal.xy >= 0.0, float2(1.0), float2(-1.0));
	return normalize(normal);
}

// a traced pixel's surface (the pre-pass's), and how near an emitter point counts (two pixels' widths there)
bool EmitterSurfaceAt(int2 pixel, out ViewData view, out ShadowSurface surface, out float nearest2)
{
	nearest2 = 0.0;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, surface))
		return false;
	float nearest = 2.0 * surface.metersPerPixel;
	nearest2 = nearest * nearest;
	return true;
}

// how far a traced pixel's point may be from where a neighbor's (or the last frame's) was and still be the same surface:
// 2% of the distance to the eye, and the depth a stride of pixels makes along the surface (more at a slant)
float EmitterTolerance(ViewData view, ShadowSurface surface, uint stride)
{
	float3 toEye = view.eyePosition.xyz - surface.position;
	float slant = abs(dot(surface.normal, normalize(toEye)));
	return 0.02 * length(toEye) + float(stride) * surface.metersPerPixel * sqrt(max(1.0 - slant * slant, 0.0)) / max(slant, 0.05);
}

// whether a view is where it was last frame (so a traced pixel is the same point, see ComputeEmitterShade)
bool EmitterViewStill(ViewData view)
{
	return view.previousEyePosition.w > 0.0 && all(view.previousViewProjection[0] == view.viewProjection[0]) &&
		   all(view.previousViewProjection[1] == view.viewProjection[1]) && all(view.previousViewProjection[2] == view.viewProjection[2]) &&
		   all(view.previousViewProjection[3] == view.viewProjection[3]);
}

// the Emitter Sample pass: per traced pixel, its candidates resampled, then, with EmitterData::temporalReuse
// (SPEEDO_EMITTER_REUSE=1), merged with the reservoir of the traced pixel where its point was last frame (by the view's
// last view projection, if that traced pixel's point was the same surface, see EmitterTolerance: else it saw something
// else; and not where something moves, the stencil), whose count is capped (EmitterData::historyCap, so that the frame's
// candidates keep a share), into the frame's reservoirs before the spatial reuse (gEmitterReservoirs' first part). the
// reuse is off by default: a reservoir keeps its point for many frames, so the frames Emitter Temporal blends are of one
// sample rather than independent ones, and it gains little from them. against the reference (mean |log2|, still), with
// the reuse and its cap of 20 against 8 candidates and 32 frames without: Bistro's string lights at night (hundreds of
// colored bulbs and lit windows, the user's pose -906.5073,218.3646,-40.5114,-7.916,60.803) 0.235 to 0.140 (cap 4
// 0.186, cap 1 0.164: the more history, the worse), and at the end of a turn 0.334 to 0.239; Bistro's lantern pose 0.181
// to 0.089, EmissiveStrengthTest 0.064 to 0.033, CornellBox-Original 0.029 to 0.015; 4.1 ms more of Bistro's 81 (the
// rays every frame, without the reuse's known visibility, 2.3; the candidates, 1.8)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeEmitterSample(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 tracedExtent = EmitterTracedExtent();
	if (any(dispatchThreadId.xy >= tracedExtent))
		return;
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	uint index = dispatchThreadId.y * tracedExtent.x + dispatchThreadId.x;
	int2 pixel = int2(dispatchThreadId.xy) * int(data.stride);
	ViewData view;
	ShadowSurface surface;
	float nearest2;
	if (!EmitterSurfaceAt(pixel, view, surface, nearest2))
	{
		gEmitterReservoirs[index] = EmitterPack(EmitterEmpty());
		return;
	}
	EmitterRandom random = EmitterRandomAt(uint2(pixel), data.frame);
	EmitterReservoir candidates = EmitterCandidates(surface.position, surface.normal, data.candidateCount, nearest2, random);
	EmitterReservoir reservoir = EmitterEmpty();
	reservoir.Merge(candidates, candidates.target, random);
	bool moves = (gMainStencil.Load(int3(pixel, 0)) & 1u) != 0u;
	float4 clip = mul(view.previousViewProjection, float4(surface.position, 1.0));
	if (data.temporalReuse != 0u && view.previousEyePosition.w > 0.0 && !moves && clip.w > 0.0)
	{
		float2 at = (clip.xy / clip.w * 0.5 + 0.5) * view.viewport.zw + view.viewport.xy;
		int2 traced = int2(round((at - 0.5) / float(data.stride)));
		int2 last = traced * int(data.stride);
		uint previous = (data.frame & 1u) ^ 1u;
		if (all(traced >= 0) && all(traced < int2(tracedExtent)) && all(float2(last) >= view.viewport.xy) &&
			all(float2(last) < view.viewport.xy + view.viewport.zw))
		{
			float4 seen = gTextures[SHADER_TYPES_EMITTER_SURFACE_TEXTURE + previous].Load(int3(traced, 0));
			if (seen.w >= 0.0 && distance(seen.xyz, surface.position) <= EmitterTolerance(view, surface, data.stride) &&
				dot(EmitterUnpackNormal(seen.w), surface.normal) > 0.9)
			{
				uint tracedCount = tracedExtent.x * tracedExtent.y;
				uint lastIndex = uint(traced.y) * tracedExtent.x + uint(traced.x);
				EmitterReservoir history = EmitterUnpack(gEmitterReservoirs[(1u + previous) * tracedCount + lastIndex]);
				history.count = min(history.count, data.historyCap * float(data.candidateCount));
				// its point's visibility as known, if it is chosen (see ComputeEmitterShade)
				if (reservoir.Merge(history, EmitterTargetOf(history, surface.position, surface.normal, nearest2), random))
					reservoir.age = history.age;
			}
		}
	}
	reservoir.Finish();
	gEmitterReservoirs[index] = EmitterPack(reservoir);
}

// the Emitter Shade pass: per traced pixel, its reservoir merged with those of EmitterData::spatialCount traced pixels
// around (within spatialRadius of them, on the same surface: their points within EmitterTolerance of its tangent plane,
// their normals within 25 degrees); its point's light, shadowed by its ray from off the surface (by a pixel and a half,
// more far from the eye, as the traced shadows'), into the raw image; and the reservoir into the part of the frame's
// parity, which the next frame's Emitter Sample reuses, and the traced pixel's surface into the image of the frame's
// parity, which Emitter Temporal upsamples by and the next frame's Emitter Sample checks its reuse against
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeEmitterShade(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 tracedExtent = EmitterTracedExtent();
	if (any(dispatchThreadId.xy >= tracedExtent))
		return;
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	uint tracedCount = tracedExtent.x * tracedExtent.y;
	uint index = dispatchThreadId.y * tracedExtent.x + dispatchThreadId.x;
	uint part = 1u + (data.frame & 1u);
	int2 traced = int2(dispatchThreadId.xy);
	int2 pixel = traced * int(data.stride);
	RWTexture2D raw = gRWTextures[SHADER_TYPES_EMITTER_RAW_RW_TEXTURE];
	RWTexture2D surfaces = gRWTextures[SHADER_TYPES_EMITTER_SURFACE_RW_TEXTURE + (data.frame & 1u)];
	ViewData view;
	ShadowSurface surface;
	float nearest2;
	if (!EmitterSurfaceAt(pixel, view, surface, nearest2))
	{
		gEmitterReservoirs[part * tracedCount + index] = EmitterPack(EmitterEmpty());
		raw[traced] = float4(0.0, 0.0, 0.0, -1.0);
		surfaces[traced] = float4(0.0, 0.0, 0.0, -1.0);
		return;
	}
	surfaces[traced] = float4(surface.position, EmitterPackNormal(surface.normal));
	EmitterRandom random = EmitterRandomAt(uint2(pixel), data.frame ^ 0x5bd1e995u);
	// the reservoirs merged: the pixel's, then its neighbors', and the surfaces they were made for
	EmitterReservoir merged[1 + kEmitterSpatialCountMost];
	float3 positions[1 + kEmitterSpatialCountMost];
	float3 normals[1 + kEmitterSpatialCountMost];
	merged[0] = EmitterUnpack(gEmitterReservoirs[index]);
	positions[0] = surface.position;
	normals[0] = surface.normal;
	uint mergedCount = 1;
	float tolerance = EmitterTolerance(view, surface, data.stride);
	for (uint neighborIt = 0; neighborIt < min(data.spatialCount, kEmitterSpatialCountMost); neighborIt++)
	{
		float radius = data.spatialRadius * sqrt(random.Next());
		float angle = random.Next() * 6.28318530718;
		int2 neighbor = traced + int2(round(radius * float2(cos(angle), sin(angle))));
		int2 at = neighbor * int(data.stride);
		if (all(neighbor == traced) || any(neighbor < 0) || any(neighbor >= int2(tracedExtent)) || any(float2(at) < view.viewport.xy) ||
			any(float2(at) >= view.viewport.xy + view.viewport.zw))
			continue;
		float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(at, 0)).r;
		if (depth >= 1.0)
			continue;
		float3 position = ShadowPosition(view, float2(at), depth);
		float3 normal = normalize(gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(at, 0)).xyz * 2.0 - 1.0);
		if (dot(normal, surface.normal) < 0.9 || abs(dot(position - surface.position, surface.normal)) > tolerance)
			continue;
		merged[mergedCount] = EmitterUnpack(gEmitterReservoirs[uint(neighbor.y) * tracedExtent.x + uint(neighbor.x)]);
		positions[mergedCount] = position;
		normals[mergedCount] = normal;
		mergedCount++;
	}
	// each by the generalized balance heuristic (Talbot's MIS among the reservoirs, as ReSTIR's unbiased reuse has it, of
	// the targets alone): its point weighed by its own pixel's candidates' share of the chance any of the pixels' had
	// to pick it (their counts times their targets for it). merged by their counts alone (1/M), a neighbor that couldn't
	// have picked a point this pixel could (another facing, or the emitter's back turned to it) counted as if it had
	// tried, and the light came out 2.6% darker than the reference on EmissiveStrengthTest, 6% on Bistro's street
	EmitterReservoir reservoir = EmitterEmpty();
	for (uint mergedIt = 0; mergedIt < mergedCount; mergedIt++)
	{
		EmitterReservoir candidate = merged[mergedIt];
		reservoir.count += candidate.count;
		if (candidate.triangle == ~0u)
			continue;
		EmitterPoint point = EmitterPointAt(candidate.triangle, candidate.barycentrics);
		float luminance = EmitterLuminance(point.emission);
		float chance = 0.0;
		float own = 0.0;
		float here = 0.0;
		for (uint pixelIt = 0; pixelIt < mergedCount; pixelIt++)
		{
			float3 toPoint;
			float distance;
			float target = luminance * EmitterGeometry(positions[pixelIt], normals[pixelIt], point, nearest2, toPoint, distance);
			chance += merged[pixelIt].count * target;
			if (pixelIt == mergedIt)
				own = merged[pixelIt].count * target;
			if (pixelIt == 0u)
				here = target;
		}
		float weight = chance > 0.0 ? own / chance * here * candidate.weight : 0.0;
		// the pixel's own point keeps its visibility as known (a neighbor's is of another point)
		if (reservoir.Add(candidate.triangle, candidate.barycentrics, here, weight, random) && mergedIt == 0u)
			reservoir.age = candidate.age;
	}
	// its weight: the sum's over the chosen point's target (the heuristic's weights sum to 1: no count)
	reservoir.weight = reservoir.triangle != ~0u && reservoir.target > 0.0 ? reservoir.weight / reservoir.target : 0.0;
	if (reservoir.weight <= 0.0)
		reservoir.triangle = ~0u;
	float bias = max(1.5 * surface.metersPerPixel, 1e-4 * distance(surface.position, view.eyePosition.xyz));
	bool moving = data.movingBvh != 0u;
	bool visible = true;
	float3 irradiance = 0.0;
	// the ray, unless the point is known to reach the pixel: while the view doesn't move (the traced pixel is the same
	// point), a point found visible stays so for EmitterData::visibilityFrameCount frames, after which its ray is traced
	// again (where something moves, which may come between, after fewer); the reservoirs mostly keep their point under a
	// still camera, and the rays were most of the emitters' cost (Bistro's street, 2560x1440, every other pixel traced: 25 of 49 ms a frame)
	if (reservoir.age > 0u && reservoir.age < data.visibilityFrameCount && data.raw == 0u && EmitterViewStill(view))
	{
		irradiance = EmitterShade(reservoir, surface.position, surface.normal, bias, nearest2, moving, false, visible);
		reservoir.age++;
	}
	else
	{
		// else traced by chance where the light it would give unshadowed, as the frame shows it (a white surface's, times
		// the exposure), is below EmitterData::roulette, in proportion, and the light it finds weighed up by it (Russian
		// roulette): what wouldn't show isn't worth a ray
		float shown = reservoir.target * reservoir.weight / kPi * gPushConstants.exposure;
		float chance = data.roulette > 0.0 && data.raw == 0u ? saturate(shown / data.roulette) : 1.0;
		reservoir.age = 0u;
		if (chance >= 1.0 || random.Next() < chance)
		{
			irradiance = EmitterShade(reservoir, surface.position, surface.normal, bias, nearest2, moving, true, visible) / chance;
			reservoir.age = visible && reservoir.triangle != ~0u ? 1u : 0u;
		}
	}
	// a point its ray finds hidden is kept for the next frame's reuse as the candidates it stood for, of no light (ReSTIR's
	// visibility reuse, RTXDI's discard of invisible samples): a pixel's reservoir then holds on to what it sees. kept as
	// it was, a back wall's held a brighter cube behind the partition as often as its own, by its target alone, and half
	// its pixels were black. not with the spatial reuse: a neighbor's point hidden there but not here (across a shadow's
	// edge) counted as its candidates having found nothing, and Bistro's street came out 6% darker than the reference
	// (without the discard, the reuse is as bright as the reference, and noisier than this). dropping the hidden point's
	// count with it made the light 40% too bright: the points kept are then only those that reach the pixel, each weighed
	// for the unshadowed light. the traced pixels stay put: offset in turn over the stride's square, each frame's
	// temporal reuse was of a point beside the last, with the same bias (stride 4: 6% darker)
	if (!visible && data.spatialCount == 0u)
	{
		reservoir.triangle = ~0u;
		reservoir.weight = 0.0;
	}
	gEmitterReservoirs[part * tracedCount + index] = EmitterPack(reservoir);
	// the reference (DEBUG_VIEW_EMITTERS_REFERENCE): the mean of SHADER_TYPES_EMITTER_REFERENCE_SAMPLE_COUNT independent
	// estimates, each its own candidates resampled and its own shadow ray, nothing reused or filtered
	if (data.raw == 2u)
	{
		irradiance = 0.0;
		for (uint sampleIt = 0; sampleIt < SHADER_TYPES_EMITTER_REFERENCE_SAMPLE_COUNT; sampleIt++)
		{
			EmitterReservoir candidates = EmitterCandidates(surface.position, surface.normal, data.candidateCount, nearest2, random);
			irradiance += EmitterShade(candidates, surface.position, surface.normal, bias, nearest2, moving, true, visible);
		}
		irradiance /= float(SHADER_TYPES_EMITTER_REFERENCE_SAMPLE_COUNT);
	}
	raw[traced] = float4(irradiance / kPi, 1.0);
}

// a pass of the emitters' filter (Emitter X and Y), over the traced pixels: a traced pixel's light (or the rows' result)
// averaged along its row (or column) of traced pixels within EmitterData::filterReach pixels, by a tent of their
// distance, weighed by how much each is on its surface (its distance off its tangent plane, against a stride of pixels'
// width, and how its normal agrees, as the near field's filter, GiNearFilter), leaving out what is unknown; in the debug
// views of the rays and the reference, as it is
float4 EmitterFilter(int2 traced, int2 step, uint source)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	Texture2D input = gTextures[source];
	if (data.raw != 0u)
		return input.Load(int3(traced, 0));
	int stride = int(data.stride);
	ViewData view;
	ShadowSurface center;
	if (!ViewAt(traced * stride, view) || !ShadowSurfaceAt(traced * stride, view, center))
		return kGiNearUnknown;
	uint2 extent;
	input.GetDimensions(extent.x, extent.y);
	float2 low = view.viewport.xy;
	float2 high = view.viewport.xy + view.viewport.zw;
	int reach = max(int(data.filterReach) / stride, 1);
	float4 total = 0.0;
	float weights = 0.0;
	for (int offset = -reach; offset <= reach; offset++)
	{
		int2 at = traced + step * offset;
		int2 atPixel = at * stride;
		if (any(at < 0) || any(at >= int2(extent)) || any(float2(atPixel) < low) || any(float2(atPixel) >= high))
			continue;
		float4 value = input.Load(int3(at, 0));
		if (value.a < 0.0)
			continue;
		float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(atPixel, 0)).r;
		if (depth >= 1.0)
			continue;
		float3 position = ShadowPosition(view, float2(atPixel), depth);
		float3 normal = gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(atPixel, 0)).xyz * 2.0 - 1.0;
		float plane = abs(dot(position - center.position, center.normal)) / (2.0 * float(stride) * center.metersPerPixel);
		float facing = saturate(dot(normal, center.normal) * rsqrt(max(dot(normal, normal), 1e-8)));
		float w = exp(-plane) * pow(facing, 8.0) * (float(reach + 1) - abs(float(offset)));
		total += w * value;
		weights += w;
	}
	return weights > 0.0 ? total / weights : kGiNearUnknown;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeEmitterX(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 extent;
	gRWTextures[SHADER_TYPES_EMITTER_ROWS_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	if (any(dispatchThreadId.xy >= extent))
		return;
	int2 traced = int2(dispatchThreadId.xy);
	gRWTextures[SHADER_TYPES_EMITTER_ROWS_RW_TEXTURE][traced] = EmitterFilter(traced, int2(1, 0), SHADER_TYPES_EMITTER_RAW_TEXTURE);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeEmitterY(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 extent;
	gRWTextures[SHADER_TYPES_EMITTER_FILTERED_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	if (any(dispatchThreadId.xy >= extent))
		return;
	int2 traced = int2(dispatchThreadId.xy);
	gRWTextures[SHADER_TYPES_EMITTER_FILTERED_RW_TEXTURE][traced] = EmitterFilter(traced, int2(0, 1), SHADER_TYPES_EMITTER_ROWS_TEXTURE);
}

// the frame's filtered light at a pixel (its point and normal, the pre-pass's), from the traced pixels around (their
// surfaces as the frame's Emitter Shade wrote them): bilinear over the 2x2 around it, each by how much it is on the
// pixel's surface (its point's distance off the pixel's tangent plane, against 1% of the distance to the eye, and how its
// normal agrees), or where none is, the 3x3 around the nearest by that alone, or where none of those is either (thin
// geometry between the traced pixels: a frame's side, a ledge's edge, a mullion), the 3x3 by how their normals agree,
// never 0; kGiNearUnknown where nothing was gathered. left unknown, those pixels took no emitter light, dark dashes along
// the edges at night (Bistro's string lights, the user's pose -906.5073,218.3646,-40.5114,-7.916,60.803; magenta in the
// debug views, as white as the tonemapper makes it)
float4 EmitterUpsample(int2 pixel, float3 position, float3 normal, float3 eye, uint stride, uint current)
{
	Texture2D light = gTextures[SHADER_TYPES_EMITTER_FILTERED_TEXTURE];
	Texture2D surfaces = gTextures[SHADER_TYPES_EMITTER_SURFACE_TEXTURE + current];
	uint2 extent;
	light.GetDimensions(extent.x, extent.y);
	float scale = max(0.01 * distance(position, eye), 1e-6);
	float2 at = float2(pixel) / float(stride);
	int2 base = int2(floor(at));
	float2 f = at - float2(base);
	float3 total = 0.0;
	float weights = 0.0;
	for (int corner = 0; corner < 4; corner++)
	{
		int2 offset = int2(corner & 1, corner >> 1);
		int2 tap = base + offset;
		if (any(tap < 0) || any(tap >= int2(extent)))
			continue;
		float4 value = light.Load(int3(tap, 0));
		float4 seen = surfaces.Load(int3(tap, 0));
		if (value.a < 0.0 || seen.w < 0.0)
			continue;
		float similar = exp(-abs(dot(seen.xyz - position, normal)) / scale) * pow(saturate(dot(EmitterUnpackNormal(seen.w), normal)), 8.0);
		float w = (offset.x != 0 ? f.x : 1.0 - f.x) * (offset.y != 0 ? f.y : 1.0 - f.y) * similar;
		total += w * value.rgb;
		weights += w;
	}
	if (weights < 1e-4)
	{
		total = 0.0;
		weights = 0.0;
		float3 alikeTotal = 0.0;
		float alikeWeights = 0.0;
		int2 nearest = int2(round(at));
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++)
			{
				int2 tap = nearest + int2(x, y);
				if (any(tap < 0) || any(tap >= int2(extent)))
					continue;
				float4 value = light.Load(int3(tap, 0));
				float4 seen = surfaces.Load(int3(tap, 0));
				if (value.a < 0.0 || seen.w < 0.0)
					continue;
				float facing = saturate(dot(EmitterUnpackNormal(seen.w), normal));
				float w = exp(-abs(dot(seen.xyz - position, normal)) / scale) * pow(facing, 8.0);
				total += w * value.rgb;
				weights += w;
				alikeTotal += (facing + 0.01) * value.rgb;
				alikeWeights += facing + 0.01;
			}
		if (weights <= 1e-6)
		{
			total = alikeTotal;
			weights = alikeWeights;
		}
	}
	return weights > 0.0 ? float4(total / weights, 1.0) : kGiNearUnknown;
}

// the Emitter Temporal pass: per pixel of the pre-pass's depth, the frame's filtered light upsampled from the traced
// pixels around (EmitterUpsample), blended into what the last frame's history holds where its point was then (by the
// view's last view projection, the last jitter's): bilinear over the 4 pixels around that place, each only where its
// point was as far from the last eye (within a fifth of a percent and the depth two pixels make along the surface) and
// its normal within 25 degrees, and none where something moves (the main depth's stencil), so neither ghosts; by 1 over
// the frames it holds (its alpha), at least EmitterData::blend (1 where there is no history: the frame's alone); into
// the images of the frame's parity, with each pixel's surface (its distance from the eye and its normal), which the next
// frame checks its history by. per pixel rather than per traced pixel: under temporal upscaling's jitter, a traced
// pixel at an edge saw one surface in some frames and the other in others, and which traced pixels a pixel's upsampling
// took changed with it, a change over a block of pixels at once, which the upscaler's clamp to the 3x3 around takes for
// the frame's light: speckles on every edge, ledge and leaf the emitters light (Bistro's string lights, the user's
// evening pose -994.4661,340.8679,-53.6291,-8.370,78.594 at 19.817, the left tree's leaves, the balcony's railing; 6066
// pixels a frame changed by more than 32 of 255 in the emitters' debug view at exposure 5, against 356 without
// upscaling)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeEmitterTemporal(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	uint current = data.frame & 1u;
	RWTexture2D history = gRWTextures[SHADER_TYPES_EMITTER_HISTORY_RW_TEXTURE + current];
	RWTexture2D surfaces = gRWTextures[SHADER_TYPES_EMITTER_PIXEL_SURFACE_RW_TEXTURE + current];
	uint2 extent;
	history.GetDimensions(extent.x, extent.y);
	if (any(dispatchThreadId.xy >= extent))
		return;
	int2 pixel = int2(dispatchThreadId.xy);
	ViewData view;
	ShadowSurface surface;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, surface))
	{
		history[pixel] = kGiNearUnknown;
		surfaces[pixel] = float4(-1.0, 0.0, 0.0, 0.0);
		return;
	}
	float3 toEye = view.eyePosition.xyz - surface.position;
	surfaces[pixel] = float4(length(toEye), EmitterPackNormal(surface.normal), 0.0, 0.0);
	float4 now = EmitterUpsample(pixel, surface.position, surface.normal, view.eyePosition.xyz, data.stride, current);
	float4 result = now;
	float blend = data.raw == 1u ? 1.0 : data.blend;
	bool moves = (gMainStencil.Load(int3(pixel, 0)) & 1u) != 0u;
	float4 clip = mul(view.previousViewProjection, float4(surface.position, 1.0));
	if (now.a >= 0.0 && blend < 1.0 && view.previousEyePosition.w > 0.0 && !moves && clip.w > 0.0)
	{
		float2 at = (clip.xy / clip.w * 0.5 + 0.5) * view.viewport.zw + view.viewport.xy - 0.5;
		float expected = distance(surface.position, view.previousEyePosition.xyz);
		float slant = abs(dot(surface.normal, normalize(toEye)));
		float tolerance = 0.002 * expected + 2.0 * surface.metersPerPixel * sqrt(max(1.0 - slant * slant, 0.0)) / max(slant, 0.05);
		Texture2D last = gTextures[SHADER_TYPES_EMITTER_HISTORY_TEXTURE + (current ^ 1u)];
		Texture2D lastSurfaces = gTextures[SHADER_TYPES_EMITTER_PIXEL_SURFACE_TEXTURE + (current ^ 1u)];
		int2 base = int2(floor(at));
		float2 f = at - float2(base);
		float4 total = 0.0;
		float weights = 0.0;
		for (int corner = 0; corner < 4; corner++)
		{
			int2 offset = int2(corner & 1, corner >> 1);
			int2 tap = base + offset;
			if (any(float2(tap) < view.viewport.xy) || any(float2(tap) >= view.viewport.xy + view.viewport.zw))
				continue;
			float4 value = last.Load(int3(tap, 0));
			float2 seen = lastSurfaces.Load(int3(tap, 0)).xy;
			if (value.a < 0.0 || seen.x < 0.0 || abs(seen.x - expected) > tolerance || dot(EmitterUnpackNormal(seen.y), surface.normal) < 0.9)
				continue;
			float w = (offset.x != 0 ? f.x : 1.0 - f.x) * (offset.y != 0 ? f.y : 1.0 - f.y);
			total += w * value;
			weights += w;
		}
		// a mean of the frames the history holds, at most the history's (1 over the blend): blended by the blend alone, a
		// history started over took its first frame's light as all of it and let it go at a thirty-second a frame
		if (weights > 1e-3)
		{
			float count = min(total.a / weights + 1.0, 1.0 / blend);
			result = float4(lerp(total.rgb / weights, now.rgb, max(1.0 / count, blend)), count);
		}
	}
	history[pixel] = result;
}

// the main pass's opaque fragments' light from the emitters (see Shade): the irradiance over pi Emitter Temporal gathered
// at the fragment's pixel (whose surface the fragment is: the main list's opaque fragments shade at the pre-pass's
// depth), -1 where nothing was gathered
float3 EmitterIrradiance()
{
	if (!gFragmentEmitters)
		return -1.0;
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	float4 light = gTextures[SHADER_TYPES_EMITTER_HISTORY_TEXTURE + (data.frame & 1u)].Load(int3(gFragmentPixel, 0));
	return light.a >= 0.0 ? light.rgb : -1.0;
}
