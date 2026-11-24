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

// a pixel's light over frames (Emitter Temporal's, GI Near Temporal's, see HistoryFireflyClamp) takes a frame's as at
// most kFireflyRise times what it holds, or what it holds and kFireflyFloor of the frame's white more where that is
// more; the emitters' once their history holds kEmitterFireflyFrameCount frames
static const float kFireflyRise = 8.0;
static const float kFireflyFloor = 0.25;
static const float kEmitterFireflyFrameCount = 8.0;

// whether emissive surfaces light the scene this frame (else the indirect lighting's rays take their emission)
bool EmittersLight()
{
	return !Skipped(SHADER_TYPES_LAYERS_EMITTERS) && gEmitterData[gPushConstants.frameIndex][0].count > 0u;
}

// whether the emitters' caustics are traced this frame (see ComputeEmitterCaustics)
bool EmitterCausticsTraced()
{
	return gEmitterData[gPushConstants.frameIndex][0].causticPhotonCount > 0u;
}

float EmitterLuminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// a frame's light against what a pixel's history holds (see kFireflyRise): at most kFireflyRise times its luminance,
// or it and kFireflyFloor of the frame's white more (the white the pixel's view's exposure makes) where that is more
float3 HistoryFireflyClamp(float3 sample, float3 kept, float exposure)
{
	float keptLuminance = EmitterLuminance(kept);
	float bound = max(kFireflyRise * keptLuminance, keptLuminance + kFireflyFloor / max(exposure, 1e-6));
	float luminance = EmitterLuminance(sample);
	return luminance > bound ? sample * (bound / luminance) : sample;
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
		point.emission *= EmissiveTexel(SampleViewLevel(view, ViewTexCoord(view, texCoord01), kEmitterTextureLevel).rgb);
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
	// and the surface's own: toward the box (a point's, normal 0: whichever way, see EmitterVoxelLight)
	float cosIncident = -dot(normal, toSurface);
	float sinIncident = sqrt(max(1.0 - cosIncident * cosIncident, 0.0));
	float cosReceived = dot(normal, normal) > 0.5 ? EmitterCosineLess(sinIncident, cosIncident, sinBox, cosBox) : 1.0;
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
	// (a point's, normal 0: what arrives whichever way, see EmitterVoxelLight)
	float cosine = dot(normal, normal) > 0.5 ? dot(normal, toPoint) : 1.0;
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

// a point of an emitter's, down the tree as EmitterPick goes (each child by its importance's share of the two), but
// toward the child whose box is nearest the point (the first where both hold it) rather than at random: the probability
// of that walk, and the triangle of the leaf it ends at. what the picks' density at a point is, where the boxes don't
// overlap; a function of the point alone, so that what weighs a light sample against a ray that met the point by chance
// is the same either way (see EmitterDensity)
float EmitterWalk(float3 position, float3 normal, float3 point, out uint triangle)
{
	triangle = ~0u;
	float probability = 1.0;
	uint node = 0;
	for (uint depthIt = 0; depthIt < EMITTER_NODE_DEPTH_MOST; depthIt++)
	{
		uint bits = asuint(gEmitterTree[node].axis.w);
		if ((bits & EMITTER_NODE_LEAF) != 0u)
		{
			triangle = bits & EMITTER_NODE_INDEX_MASK;
			return probability;
		}
		uint second = bits & EMITTER_NODE_INDEX_MASK;
		EmitterNode firstNode = gEmitterTree[node + 1u];
		EmitterNode secondNode = gEmitterTree[second];
		float first = EmitterImportance(firstNode, position, normal);
		float other = EmitterImportance(secondNode, position, normal);
		if (!(first + other > 0.0))
			return 0.0;
		float3 outsideFirst = max(max(firstNode.low.xyz - point, point - firstNode.high.xyz), 0.0);
		float3 outsideSecond = max(max(secondNode.low.xyz - point, point - secondNode.high.xyz), 0.0);
		bool toFirst = dot(outsideFirst, outsideFirst) <= dot(outsideSecond, outsideSecond);
		probability *= (toFirst ? first : other) / (first + other);
		node = toFirst ? node + 1u : second;
	}
	return 0.0;
}

// the density, per solid angle, the emitters' samples have at a point of an emitter seen from a surface (at position, of
// normal): the walk's probability (EmitterWalk) uniformly over its triangle's area, by the area's solid angle (its
// distance squared over the cosine it faces the surface at); 0 where it faces away. the candidates' picks', not what the
// resampling makes of them: the balance heuristic's light density where a glossy reflection's light sample and its ray
// share the emitters' highlights (see TracedGlossyEmitters)
float EmitterDensity(float3 position, float3 normal, float3 point)
{
	uint triangle;
	float probability = EmitterWalk(position, normal, point, triangle);
	if (!(probability > 0.0))
		return 0.0;
	EmitterTriangle emitter = gEmitters[triangle];
	float3 across = cross(emitter.edge1.xyz, emitter.edge2.xyz);
	float twiceArea = max(length(across), 1e-30);
	float3 offset = point - position;
	float distance2 = dot(offset, offset);
	float cosine = -dot(across / twiceArea, offset * rsqrt(max(distance2, 1e-30)));
	if ((asuint(emitter.edge2.w) & EMITTER_FLAG_DOUBLE_SIDED) != 0u)
		cosine = abs(cosine);
	return cosine > 0.0 ? probability / (0.5 * twiceArea) * distance2 / cosine : 0.0;
}

// the cosine of the angle a solid's faces where a ray goes in and comes out differ by at most where it is a slab there (2
// degrees: a pane with a thickness, a shell), which lets the light on as it was going
static const float kEmitterSlabCosine = 0.99939083;
// and how far apart the two faces may be where the one the light goes in by is curved, of the radius that curves by (a
// shell: a quarter; normals 2 degrees apart mean a thickness of a twentieth or less): a lens's middle, its faces as far
// apart as it is thick, is no slab
static const float kEmitterShellReach = 0.25;

// whether solid glass (see TransmissionSolid) gathers the light an emitter's point sends a surface, along the ray from
// the surface (origin, direction, as far as tmax), while the emitters' caustics are traced
// (EmitterData::causticPhotonCount): the diffuse light that comes through it is theirs (ComputeEmitterCaustics), bent
// where it went in and out, which the straight line would count again where it doesn't go (cornell-caustic's floor under
// its water, lit straight through the waves and by their caustics both). the highlights keep the straight line's (the
// photons bring irradiance alone): glass-of-water's backdrop and table, metals of a roughness of 0.32, lost the softbox's
// reflection behind its glass and stream, which their glossy rays, through two glass surfaces at most, can't follow
// through the water, ice and bubbles (blurred 0.738 stops off its reference to 0.841). thin walls let it through as
// before, as the caustics' photons pass them by.
// and so does solid glass that gathers nothing, whose light the photons brought as a grain of their own (a few a pixel,
// where the shadow rays' light is filtered and sharp: the user's reports of 2026-11-23, docs/research-scenes-review-plan.md):
// - a body the emitter is inside (the ray goes into it and ends there): its light leaves spread, not gathered (a point
//   within a convex body is nearer its surface than the surface's focus). lamp's bulb, a single sheet the set's
//   conditions take as solid, around its emitter: the floor was lit by photons alone, a grain all over it
// - a slab, where the ray goes in and comes out (the faces' normals there within kEmitterSlabCosine, and where the light
//   goes in by a curved one, no further apart than kEmitterShellReach): the light goes on as it was going. modern-hall's
//   glass rail between its wall lights and the wall, spaceship's dome (a shell) over its lights
// - a flat face the light goes in by (GlassSurface::flat), to a surface inside (the first solid face the ray meets is a
//   back face) or out by another flat face: a pane taken as a solid, a box's corner, bend the light, all of it alike (the
//   rail's edges threw a few photons each a speck)
// the photons follow the same rule (ComputeEmitterCaustics): what the ray lets through they don't bring. the glass along
// the way, nearest the surface first (a query each, and one that meets nothing), at most
// SHADER_TYPES_CAUSTIC_SURFACE_COUNT surfaces (past them, taken as through). from the surface, as the ray was before the
// rule: a query of any glass from the emitter's point cost cornell-caustic's Main Transmissive (its refractions' hits'
// light samples, TracedEmitterHidden) 4.15 ms a frame at 1440p where the same from the surface cost 3.35.
// the rule costs that pass 0.6 ms there (3.3 to 3.95; glass-of-water's 11.3 to 13.3), and it is the fragment shader's
// size, not what it traces: with the other face looked for only near a curved one, with the scene's and the glass's
// queries made one, or with any glass queried first and a curved back face ending it (a query fewer than before the
// rule), it measured 3.9 to 3.95 each time. the hits' light sample out of the fragment shader would take it back
// (docs/backlog.md). (EmitterGatheredByGlass, below the walk's state and step)

// the rule's state along a ray from a surface toward an emitter's point
struct EmitterGlassWalk
{
	bool first; // no solid face met yet
	bool inside; // in a solid the ray went into
	float3 farNormal; // of the face it went in by, where the light comes out
	bool farFlat;
	float thickness; // since that face
};

EmitterGlassWalk EmitterGlassWalkStart()
{
	EmitterGlassWalk walk;
	walk.first = true;
	walk.inside = false;
	walk.farNormal = 0.0;
	walk.farFlat = false;
	walk.thickness = 0.0;
	return walk;
}

// the next transmissive face a walk's ray met (hit, step further along it): whether the glass gathers the light there,
// which ends the walk
bool EmitterGlassWalkStep(inout EmitterGlassWalk walk, BvhHit hit, float step, uint frameIndex)
{
	walk.thickness += step;
	if (!TransmissionSolid(gMaterialData[hit.flags >> BVH_TRIANGLE_MATERIAL_SHIFT].transmission.y))
		return false;
	bool flat;
	float radius;
	float3 normal = GlassNormalAt(hit, frameIndex, flat, radius);
	if (hit.frontFace)
	{
		if (walk.inside) // (a body inside a body: the photons')
			return true;
		walk.inside = true;
		walk.farNormal = normal;
		walk.farFlat = flat;
		walk.thickness = 0.0;
	}
	else if (walk.inside)
	{
		// out by the face the light went in by
		bool slab = dot(walk.farNormal, normal) >= kEmitterSlabCosine && (flat || walk.thickness <= kEmitterShellReach * radius);
		if (!(slab || (flat && walk.farFlat)))
			return true;
		walk.inside = false;
	}
	else if (walk.first && !flat) // the surface is inside the solid, the light into it by a curved face
		return true;
	walk.first = false;
	return false;
}

// (the rule above)
bool EmitterGatheredByGlass(float3 origin, float3 direction, float tmax)
{
	if (!EmitterCausticsTraced())
		return false;
	uint frameIndex = gPushConstants.frameIndex;
	EmitterGlassWalk walk = EmitterGlassWalkStart();
	float3 from = origin;
	float left = tmax;
	for (uint surfaceIt = 0; surfaceIt < GlassSurfaceCount(); surfaceIt++)
	{
		BvhHit hit = BvhTraceGlass(from, direction, left);
		if (!BvhHitSomething(hit))
			break;
		float3 at = from + direction * hit.t;
		float step = hit.t + max(1e-4, 1e-5 * max(abs(at.x), max(abs(at.y), abs(at.z))));
		if (EmitterGlassWalkStep(walk, hit, step, frameIndex))
			return true;
		from += direction * step;
		left -= step;
		if (left <= 0.0)
			break;
	}
	return false;
}

// the irradiance a finished reservoir's point gives a surface, shadowed: its ray through the BVHs (alpha masks cut out;
// what moves with moving), from off the surface by bias toward the point, ending short of it (the emitter's own
// triangle), unless it is known to reach the surface (!trace); and whether it does (visible). with glass, also whether
// solid glass on its way gathers it while the emitters' caustics are traced (throughGlass, see EmitterGatheredByGlass):
// the light the caustics bring a surface's diffuse in its place
float3 EmitterShade(
	EmitterReservoir reservoir, float3 position, float3 normal, float bias, float nearest2, bool moving, bool trace, bool glass, out bool visible,
	out bool throughGlass)
{
	visible = false;
	throughGlass = false;
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
	throughGlass = trace && glass && EmitterGatheredByGlass(origin, direction / max(span, 1e-30), span * 0.999);
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

// the light the emitters give a voxel as what a surface of any normal there takes of it (see GiKeptIrradiance): what
// arrives (the return: each sample's emission by what it gives a surface facing it), and the way it arrives from
// (toward: the samples' directions' mean by their light, shorter than 1 the wider they are apart). normal: the voxel's,
// its samples those in front of it as EmitterVoxelIrradiance's; or 0, from all around, for a voxel that holds surfaces
// facing several ways (a corner's)
float3 EmitterVoxelLight(GiData gi, float3 position, float3 normal, bool moving, uint2 seed, out float3 toward)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	return EmitterLightSamples(gi, position, normal, moving, seed, moving ? data.frame : 0u, data.voxelSampleCount, data.voxelCandidateCount, toward);
}

// (count samples, their numbers seed's and turn's: the voxels' at their voxelizing, EmitterVoxelLight, and one at a time
// at a surface for the surface cache's entries, which keep their mean: see GiCacheUpdate)
float3 EmitterLightSamples(GiData gi, float3 position, float3 normal, bool moving, uint2 seed, uint turn, uint count, uint candidateCount, out float3 toward)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	float voxel = gi.volumeOrigin.w;
	float nearest2 = voxel * voxel;
	bool bvh = GiTracesBvh(gi);
	bool sided = dot(normal, normal) > 0.5;
	EmitterRandom random = EmitterRandomAt(seed, turn);
	float3 light = 0.0;
	toward = 0.0;
	for (uint sampleIt = 0; sampleIt < count; sampleIt++)
	{
		EmitterReservoir reservoir = EmitterCandidates(position, normal, candidateCount, nearest2, random);
		if (reservoir.triangle == ~0u)
			continue;
		EmitterPoint point = EmitterPointAt(reservoir.triangle, reservoir.barycentrics);
		float3 toPoint;
		float distance;
		float geometry = EmitterGeometry(position, normal, point, nearest2, toPoint, distance);
		float reach = bvh ? distance * 0.999 : distance - 1.5 * voxel;
		if (geometry <= 0.0 || reach <= 0.0)
			continue;
		// (a sided sample's geometry holds the voxel's cosine: out of it, what a surface facing the emitter would take)
		float facing = sided ? max(dot(normal, toPoint), 1e-3) : 1.0;
		float3 sample = point.emission * (geometry / facing * reservoir.weight * GiVisibility(gi, position, toPoint, reach, moving));
		light += sample;
		toward += toPoint * EmitterLuminance(sample);
	}
	if (count == 0u)
		return 0.0;
	light /= float(count);
	toward /= max(EmitterLuminance(light) * float(count), 1e-30);
	return light;
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

// where a pixel's history is in the last frame's images (PixelHistory's at, in pixels, their centers whole): the pixel
// moved as its point did (clip: the point by the view's last view projection), both frames' jitters taken out, as
// temporal upscaling's motion is (UpscaleMotion). a pixel's history holds what its jittered samples saw over frames,
// all of its area, so a still view's is the pixel's own; through the last jittered view projection alone, as it was,
// a still view's pixel was the last frame's moved by the two jitters' difference, a sub-pixel offset another way every
// frame: every frame's history was resampled bilinearly there, blurred and shifted back and forth, which crept over the
// walls however many frames it held (the user's night pose in Bistro's street, with 256 frames 0.82 of 255 over 20
// frames, 1024 0.77)
float2 PixelHistoryAt(ViewData view, float4 clip)
{
	float2 ndc = clip.xy / clip.w - view.jitter.zw + view.jitter.xy;
	return (ndc * 0.5 + 0.5) * view.viewport.zw + view.viewport.xy - 0.5;
}

// a pixel's history over frames (Emitter Temporal's, GI Near Temporal's): the last frame's light at where the pixel's
// point was then (at, PixelHistoryAt), bilinear over the 4 pixels around, each only where it saw the
// pixel's surface (lastSurfaces: its distance from the last eye within tolerance of expected, and its normal within 25
// degrees), or where none of them did, the 3x3 around that did, by their nearness; the sum and its weights (0: none).
// the 3x3 is for edges under temporal upscaling's jitter: a pixel at an edge sees one surface in some frames and the
// other in others, where the 4 around may all have seen the other last frame, and its history started over from the
// frame's light alone, as noisy as a frame is: streaks along every window frame and pilaster that crept as the frames
// replaced them (the user's night pose in Bistro's street, -752.4570,186.6341,97.5802,-1.211,63.522). held: the sum of
// the surfaces' third channels by the same weights (the near field's frame counts; the emitters' are in their light's alpha)
float4 PixelHistory(Texture2D last, Texture2D lastSurfaces, ViewData view, float2 at, float expected, float tolerance, float3 normal, out float weights, out float held)
{
	int2 base = int2(floor(at));
	float2 f = at - float2(base);
	float4 total = 0.0;
	weights = 0.0;
	held = 0.0;
	for (int corner = 0; corner < 4; corner++)
	{
		int2 offset = int2(corner & 1, corner >> 1);
		int2 tap = base + offset;
		if (any(float2(tap) < view.viewport.xy) || any(float2(tap) >= view.viewport.xy + view.viewport.zw))
			continue;
		float4 value = last.Load(int3(tap, 0));
		float3 seen = lastSurfaces.Load(int3(tap, 0)).xyz;
		if (value.a < 0.0 || seen.x < 0.0 || abs(seen.x - expected) > tolerance || dot(EmitterUnpackNormal(seen.y), normal) < 0.9)
			continue;
		float w = (offset.x != 0 ? f.x : 1.0 - f.x) * (offset.y != 0 ? f.y : 1.0 - f.y);
		total += w * value;
		weights += w;
		held += w * seen.z;
	}
	if (weights > 1e-3)
		return total;
	total = 0.0;
	weights = 0.0;
	held = 0.0;
	int2 center = int2(round(at));
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			int2 tap = center + int2(x, y);
			if (any(float2(tap) < view.viewport.xy) || any(float2(tap) >= view.viewport.xy + view.viewport.zw))
				continue;
			float4 value = last.Load(int3(tap, 0));
			float3 seen = lastSurfaces.Load(int3(tap, 0)).xyz;
			if (value.a < 0.0 || seen.x < 0.0 || abs(seen.x - expected) > tolerance || dot(EmitterUnpackNormal(seen.y), normal) < 0.9)
				continue;
			float2 d = float2(tap) - at;
			float w = 1.0 / (1.0 + dot(d, d));
			total += w * value;
			weights += w;
			held += w * seen.z;
		}
	return total;
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

// whether a view is where it was last frame (so a traced pixel is the same point, but for temporal upscaling's jitter,
// see ComputeEmitterShade): Views::Upload marks it (ViewData::previousEyePosition's w), by its view projection without
// the jitter. comparing the view projections here, as this did, never found a view still under upscaling: the jittered
// ones differ every frame
bool EmitterViewStill(ViewData view)
{
	return view.previousEyePosition.w > 1.5;
}

// how many samples a traced pixel's light is the mean of in a frame where the view jumped, so that every pixel is new
// (at most: EmitterData::newSampleCount is what a newly seen pixel's is otherwise, see ComputeEmitterNew); and the raw
// image's alpha where Emitter Shade found a pixel newly seen, until Emitter New has made it that mean (no count of
// samples is a half)
static const uint kEmitterJumpSampleCount = 4u;
static const float kEmitterNewPending = 0.5;

// whether a point the view sees is newly seen: the last frame's pixels where it was then (PixelHistoryAt, the 3x3
// around, as PixelHistory's) were off the screen, or all saw something nearer (it was hidden). by where it was alone,
// not by how many samples its history holds: along thin geometry and edges (a window's frame, a ledge, a railing) a
// pixel's history starts over most frames, whichever surface its jittered sample met and whatever its normal map gave,
// and with a count below EmitterData::newSampleCount for new, a fifth of Bistro's traced pixels took all of the samples
// every frame, a still view's too (Emitter Shade 0.10 ms to 1.22). so not where one of the last frame's pixels saw this
// depth (whatever its normal, and whatever its history holds), nor where one saw something farther or the sky (a thin
// thing its sample missed then)
bool EmitterSurfaceNew(ViewData view, ShadowSurface surface, uint current)
{
	float4 clip = mul(view.previousViewProjection, float4(surface.position, 1.0));
	if (clip.w <= 0.0)
		return true;
	float3 toEye = view.eyePosition.xyz - surface.position;
	float expected = distance(surface.position, view.previousEyePosition.xyz);
	float slant = abs(dot(surface.normal, normalize(toEye)));
	float tolerance = 0.002 * expected + 2.0 * surface.metersPerPixel * sqrt(max(1.0 - slant * slant, 0.0)) / max(slant, 0.05);
	Texture2D lastSurfaces = gTextures[SHADER_TYPES_EMITTER_PIXEL_SURFACE_TEXTURE + (current ^ 1u)];
	int2 center = int2(round(PixelHistoryAt(view, clip)));
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			int2 tap = center + int2(x, y);
			if (any(float2(tap) < view.viewport.xy) || any(float2(tap) >= view.viewport.xy + view.viewport.zw))
				continue;
			float seen = lastSurfaces.Load(int3(tap, 0)).x;
			if (seen < 0.0 || seen > expected - tolerance)
				return false;
		}
	return true;
}

// whether a traced pixel is one of a region newly seen: its point is (EmitterSurfaceNew), and those of at least three
// of the four traced pixels beside it are (one off the screen or of the sky counts): what a turn brings in at the
// screen's edge, what comes out from behind a building. not a pixel alone or a row of them: a gap its jittered sample
// looked through, a sliver beside an edge. those gain little, their light being the filter's over the traced pixels
// around, which hold one sample each, and cost most: a thread waits for the slowest of its wave, and with them a third
// of the waves had one (Emitter Shade 0.10 ms to 0.87 in a still view, 1.44 turning). nothing is new in a still view,
// and every pixel where the view jumped
bool EmitterNew(ViewData view, ShadowSurface surface, int2 pixel, uint stride, uint current)
{
	if (view.previousEyePosition.w <= 0.0)
		return true;
	if (EmitterViewStill(view) || (gMainStencil.Load(int3(pixel, 0)) & 1u) != 0u || !EmitterSurfaceNew(view, surface, current))
		return false;
	uint newCount = 0;
	for (uint sideIt = 0; sideIt < 4; sideIt++)
	{
		int2 beside = pixel + int2(sideIt == 0u ? -1 : (sideIt == 1u ? 1 : 0), sideIt == 2u ? -1 : (sideIt == 3u ? 1 : 0)) * int(stride);
		ShadowSurface other;
		if (any(float2(beside) < view.viewport.xy) || any(float2(beside) >= view.viewport.xy + view.viewport.zw) ||
			!ShadowSurfaceAt(beside, view, other) || EmitterSurfaceNew(view, other, current))
			newCount++;
	}
	return newCount >= 3u;
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

// gEmitterReservoirs' part of the frame's points, where Emitter Shade found them reaching their traced pixels (see
// EmitterPlacePoint), and the next part, their light
static const uint kEmitterPointsPart = 3u;

// a traced pixel's point, as Emitter Shade found it reaching the pixel (its reservoir's, or none), for the untraced
// specular's highlights (EmitterSpecular): where it is and its normal (packed), and its emission times its contribution
// weight and what it counts for (scale: see ComputeEmitterShade's placed), and 1 (2 double sided; 0 for none). the
// point's own fetch (its triangle, material, vertices and texture) at each of the four traced pixels a fragment takes
// cost Bistro at night 0.07 ms a frame more at 1440p
void EmitterPlacePoint(uint index, uint tracedCount, EmitterReservoir reservoir, float scale)
{
	uint4 light = 0u;
	if (reservoir.triangle != ~0u)
	{
		EmitterPoint point = EmitterPointAt(reservoir.triangle, reservoir.barycentrics);
		gEmitterReservoirs[kEmitterPointsPart * tracedCount + index] = uint4(asuint(point.position), asuint(EmitterPackNormal(point.normal)));
		light = uint4(asuint(point.emission * reservoir.weight * scale), point.doubleSided ? 2u : 1u);
	}
	gEmitterReservoirs[(kEmitterPointsPart + 1u) * tracedCount + index] = light;
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
		EmitterPlacePoint(index, tracedCount, EmitterEmpty(), 0.0);
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
		if (depth <= 0.0)
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
	// what the point's light counts for as it was found (EmitterPlacePoint): once, 0 where its ray was a gamble lost (its
	// visibility unknown, the frame takes none of its light), over the chance where won
	float placed = 1.0;
	// the ray, unless the point is known to reach the pixel: while the view doesn't move (the traced pixel is the same
	// point), a point found visible stays so for EmitterData::visibilityFrameCount frames, after which its ray is traced
	// again (where something moves, which may come between, after fewer); the reservoirs mostly keep their point under a
	// still camera, and the rays were most of the emitters' cost (Bistro's street, 2560x1440, every other pixel traced: 25 of 49 ms a frame)
	// (a point behind glass that gathers its light reaches it for the highlights alone, see EmitterGatheredByGlass: its ray is traced
	// every frame, its age kept at 0)
	bool throughGlass = false;
	if (reservoir.age > 0u && reservoir.age < data.visibilityFrameCount && data.raw == 0u && EmitterViewStill(view))
	{
		irradiance = EmitterShade(reservoir, surface.position, surface.normal, bias, nearest2, moving, false, false, visible, throughGlass);
		reservoir.age++;
	}
	else
	{
		// else traced by chance where the light it would give unshadowed, as the frame shows it (a white surface's, times
		// the exposure), is below EmitterData::roulette, in proportion, and the light it finds weighed up by it (Russian
		// roulette): what wouldn't show isn't worth a ray
		float shown = reservoir.target * reservoir.weight / kPi * view.exposure;
		float chance = data.roulette > 0.0 && data.raw == 0u ? saturate(shown / data.roulette) : 1.0;
		reservoir.age = 0u;
		placed = 0.0;
		if (chance >= 1.0 || random.Next() < chance)
		{
			irradiance = EmitterShade(reservoir, surface.position, surface.normal, bias, nearest2, moving, true, true, visible, throughGlass) / chance;
			reservoir.age = visible && !throughGlass && reservoir.triangle != ~0u ? 1u : 0u;
			placed = 1.0 / chance;
		}
	}
	// (the point's light for the highlights, EmitterPlacePoint's, as is; the diffuse's none)
	float3 diffuse = throughGlass ? 0.0 : irradiance;
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
	EmitterPlacePoint(index, tracedCount, visible && placed > 0.0 ? reservoir : EmitterEmpty(), placed);
	// the samples the light is the mean of go with it (the raw image's alpha), which the filter and the frames weigh it
	// by: one, but a newly seen traced pixel's (see EmitterNew), marked here for the Emitter New pass to take more
	float sampleCount = data.raw == 0u && data.newSampleCount > 1u && EmitterNew(view, surface, pixel, data.stride, data.frame & 1u) ? kEmitterNewPending : 1.0;
	// the reference (DEBUG_VIEW_EMITTERS_REFERENCE): the mean of SHADER_TYPES_EMITTER_REFERENCE_SAMPLE_COUNT independent
	// estimates, each its own candidates resampled and its own shadow ray, nothing reused or filtered
	if (data.raw == 2u)
	{
		diffuse = 0.0;
		for (uint sampleIt = 0; sampleIt < SHADER_TYPES_EMITTER_REFERENCE_SAMPLE_COUNT; sampleIt++)
		{
			EmitterReservoir candidates = EmitterCandidates(surface.position, surface.normal, data.candidateCount, nearest2, random);
			float3 sample = EmitterShade(candidates, surface.position, surface.normal, bias, nearest2, moving, true, true, visible, throughGlass);
			diffuse += throughGlass ? 0.0 : sample;
		}
		diffuse /= float(SHADER_TYPES_EMITTER_REFERENCE_SAMPLE_COUNT);
	}
	raw[traced] = float4(diffuse / kPi, sampleCount);
}

// the Emitter New pass, a group per traced pixel: where Emitter Shade marked its point newly seen (see EmitterNew), its
// light becomes the mean of EmitterData::newSampleCount samples (Emitter Shade's and the rest here; where the view
// jumped, so that every pixel is new, at most kEmitterJumpSampleCount), each its own candidates
// (EmitterData::newCandidateCount) and shadow ray, as the reference's, with their count in its alpha. one sample
// filtered over the traced pixels around is all a newly seen pixel had, and what came into view in a turn was blotches a
// filter's reach wide until its frames added up (Bistro's facades at night, the user's pose
// 724.7479,659.6074,-2775.4233,-21.034,93.844 at 02:14, turning 1.5 degrees a frame). a thread per sample
// (SHADER_TYPES_EMITTER_NEW_THREAD_COUNT of them, in turns where there are more), summed over the group: taken one after
// another by the pixel's thread of Emitter Shade, the strip a turn brings in (3% of the traced pixels) held that pass
// for as long as its samples took in a row, while most of the gpu had nothing to do (0.10 ms to 1.45 with 16 samples,
// 0.71 with 8; a sample's candidates are most of it, each twice its ray, and fewer of them was no better a trade: 32
// samples of 2 were as noisy as 8 of 8, and took longer)
groupshared float3 gEmitterNewShared[SHADER_TYPES_EMITTER_NEW_THREAD_COUNT];

[shader("compute")]
[numthreads(SHADER_TYPES_EMITTER_NEW_THREAD_COUNT, 1, 1)]
void ComputeEmitterNew(uint3 groupId : SV_GroupID, uint thread : SV_GroupIndex)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	RWTexture2D raw = gRWTextures[SHADER_TYPES_EMITTER_RAW_RW_TEXTURE];
	int2 traced = int2(groupId.xy);
	float4 first = float4(0.0, 0.0, 0.0, 1.0);
	if (all(groupId.xy < EmitterTracedExtent()))
		first = raw[traced];
	bool pending = first.a == kEmitterNewPending;
	float3 sum = 0.0;
	uint count = 0;
	if (pending)
	{
		int2 pixel = traced * int(data.stride);
		ViewData view;
		ShadowSurface surface;
		float nearest2;
		if (EmitterSurfaceAt(pixel, view, surface, nearest2))
		{
			count = (view.previousEyePosition.w > 0.0 ? data.newSampleCount : min(data.newSampleCount, kEmitterJumpSampleCount)) - 1u;
			float bias = max(1.5 * surface.metersPerPixel, 1e-4 * distance(surface.position, view.eyePosition.xyz));
			EmitterRandom random = EmitterRandomAt(uint2(pixel), (data.frame ^ 0x7f4a7c15u) + (thread + 1u) * 0x632be5abu);
			for (uint sampleIt = thread; sampleIt < count; sampleIt += SHADER_TYPES_EMITTER_NEW_THREAD_COUNT)
			{
				bool visible;
				bool throughGlass;
				EmitterReservoir candidates = EmitterCandidates(surface.position, surface.normal, data.newCandidateCount, nearest2, random);
				float3 sample = EmitterShade(candidates, surface.position, surface.normal, bias, nearest2, data.movingBvh != 0u, true, true, visible, throughGlass);
				sum += throughGlass ? 0.0 : sample;
			}
		}
	}
	gEmitterNewShared[thread] = sum;
	GroupMemoryBarrierWithGroupSync();
	if (pending && thread == 0u)
	{
		float3 total = 0.0;
		for (uint threadIt = 0; threadIt < SHADER_TYPES_EMITTER_NEW_THREAD_COUNT; threadIt++)
			total += gEmitterNewShared[threadIt];
		float samples = float(1u + count);
		raw[traced] = float4((first.rgb + total / kPi) / samples, samples);
	}
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
	float3 total = 0.0;
	float weights = 0.0;
	float samples = 0.0;
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
		if (depth <= 0.0)
			continue;
		float3 position = ShadowPosition(view, float2(atPixel), depth);
		float3 normal = gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(atPixel, 0)).xyz * 2.0 - 1.0;
		float plane = abs(dot(position - center.position, center.normal)) / (2.0 * float(stride) * center.metersPerPixel);
		float facing = saturate(dot(normal, center.normal) * rsqrt(max(dot(normal, normal), 1e-8)));
		float w = exp(-plane) * pow(facing, 8.0) * (float(reach + 1) - abs(float(offset)));
		// each by the samples it is the mean of (its alpha: a newly seen traced pixel's is of more, see
		// ComputeEmitterShade), and the result's the mean of theirs
		total += w * value.a * value.rgb;
		weights += w;
		samples += w * value.a;
	}
	return samples > 0.0 ? float4(total / samples, samples / weights) : kGiNearUnknown;
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
	float samples = 0.0;
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
		samples += w * value.a;
	}
	if (weights < 1e-4)
	{
		total = 0.0;
		weights = 0.0;
		samples = 0.0;
		float3 alikeTotal = 0.0;
		float alikeWeights = 0.0;
		float alikeSamples = 0.0;
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
				samples += w * value.a;
				alikeTotal += (facing + 0.01) * value.rgb;
				alikeWeights += facing + 0.01;
				alikeSamples += (facing + 0.01) * value.a;
			}
		if (weights <= 1e-6)
		{
			total = alikeTotal;
			weights = alikeWeights;
			samples = alikeSamples;
		}
	}
	// with the samples the traced pixels' light is the mean of (their alpha, see ComputeEmitterShade), by the same weights
	return weights > 0.0 ? float4(total / weights, samples / weights) : kGiNearUnknown;
}

// the Emitter Temporal pass: per pixel of the pre-pass's depth, the frame's filtered light upsampled from the traced
// pixels around (EmitterUpsample), blended into what the last frame's history holds where its point was then (by the
// view's last view projection, the jitters taken out: see PixelHistoryAt): bilinear over the 4 pixels around that
// place, each only where its point was as far from the last eye (within a fifth of a percent and the depth two pixels
// make along the surface) and its normal within 25 degrees, else the 3x3 around that are (see PixelHistory), and none
// where something moves (the main depth's stencil), so neither ghosts; by 1 over the frames it holds (its alpha), at least EmitterData::blend
// (EmitterData::stillBlend while the view is still; 1 where there is no history: the frame's alone); into
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
	// the last frame's highlights on the untraced specular, gathered where the pixel's point was then for the visibility
	// buffer's shading to blend the frame's into (see EmitterSpecularOverFrames), by the diffuse's reprojection: -1 where
	// there are none
	RWTexture2D specular = gRWTextures[SHADER_TYPES_EMITTER_SPECULAR_RW_TEXTURE + current];
	float4 specularKept = float4(0.0, 0.0, 0.0, -1.0);
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, surface))
	{
		history[pixel] = kGiNearUnknown;
		surfaces[pixel] = float4(-1.0, 0.0, 0.0, 0.0);
		specular[pixel] = specularKept;
		return;
	}
	float3 toEye = view.eyePosition.xyz - surface.position;
	surfaces[pixel] = float4(length(toEye), EmitterPackNormal(surface.normal), 0.0, 0.0);
	float4 now = EmitterUpsample(pixel, surface.position, surface.normal, view.eyePosition.xyz, data.stride, current);
	// (the emitters' caustics' light isn't here: the photons' cells hold it over frames, and the readers of this add it,
	// see EmitterCausticsAt)
	float4 result = now;
	float blend = data.raw == 1u ? 1.0 : data.blend;
	bool moves = (gMainStencil.Load(int3(pixel, 0)) & 1u) != 0u;
	float4 clip = mul(view.previousViewProjection, float4(surface.position, 1.0));
	if (now.a >= 0.0 && blend < 1.0 && view.previousEyePosition.w > 0.0 && !moves && clip.w > 0.0)
	{
		float2 at = PixelHistoryAt(view, clip);
		float expected = distance(surface.position, view.previousEyePosition.xyz);
		float slant = abs(dot(surface.normal, normalize(toEye)));
		float tolerance = 0.002 * expected + 2.0 * surface.metersPerPixel * sqrt(max(1.0 - slant * slant, 0.0)) / max(slant, 0.05);
		Texture2D last = gTextures[SHADER_TYPES_EMITTER_HISTORY_TEXTURE + (current ^ 1u)];
		Texture2D lastSurfaces = gTextures[SHADER_TYPES_EMITTER_PIXEL_SURFACE_TEXTURE + (current ^ 1u)];
		float weights;
		float held;
		float4 total = PixelHistory(last, lastSurfaces, view, at, expected, tolerance, surface.normal, weights, held);
		float specularWeights;
		float specularHeld;
		float4 specularTotal = PixelHistory(
			gTextures[SHADER_TYPES_EMITTER_SPECULAR_TEXTURE + (current ^ 1u)], lastSurfaces, view, at, expected, tolerance, surface.normal,
			specularWeights, specularHeld);
		if (specularWeights > 1e-3)
			specularKept = specularTotal / specularWeights;
		// a mean of the frames the history holds, at most the history's (1 over the blend): blended by the blend alone, a
		// history started over took its first frame's light as all of it and let it go at a thirty-second a frame. while
		// the view is still and nothing may change the emitters' light, the history grows to EmitterData::stillBlend's
		// frames, back to the blend's once the view moves: a mean of the last 32 frames' samples never settles, and what
		// is left of their noise wanders at the pace the frames replace it, which a still view shows as light creeping over
		// the walls (the user's night pose in Bistro's street, -752.4570,186.6341,97.5802,-1.211,63.522 at 00:57, a near
		// field stride of 4 and performance: what was drawn changed by a mean of 1.74 of 255 over 20 frames, 1.65 of it
		// the emitters' light)
		float least = data.raw == 0u && EmitterViewStill(view) ? data.stillBlend : blend;
		if (weights > 1e-3)
		{
			float3 kept = total.rgb / weights;
			float count = min(total.a / weights + now.a, 1.0 / least);
			// and a frame far brighter than what a settled history holds taken as at most a few times that (see
			// kFireflyRise): a point its candidates favour (unshadowed, near) but which seldom reaches it, an emissive
			// window's interior beside its curtain, is the whole unshadowed light the once in tens of frames its ray gets
			// through, which a 32nd of drew a blob that faded over 32 frames (Bistro's curtains at night, the user's pose
			// -827.3380,155.1481,33.8635,-9.897,72.297). the reference is 11% brighter there than the frames (8% without), the
			// light those rare frames carry, which this leaves out, and the whole frame within 0.1% of it (at 4 times: 19% there,
			// 1% over the frame)
			float3 sample = count > kEmitterFireflyFrameCount && data.raw == 0u ? HistoryFireflyClamp(now.rgb, kept, view.exposure) : now.rgb;
			result = float4(lerp(kept, sample, saturate(max(now.a / count, least))), count);
		}
	}
	history[pixel] = result;
	specular[pixel] = specularKept;
}

// the main pass's opaque fragments' light from the emitters (see Shade): the irradiance over pi Emitter Temporal gathered
// at the fragment's pixel (whose surface the fragment is: the main list's opaque fragments shade at the pre-pass's
// depth) and their caustics' there (EmitterCausticsAt), -1 where nothing was gathered
float3 EmitterIrradiance()
{
	if (!gFragmentEmitters)
		return -1.0;
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	float4 light = gTextures[SHADER_TYPES_EMITTER_HISTORY_TEXTURE + (data.frame & 1u)].Load(int3(gFragmentPixel, 0));
	return light.a >= 0.0 ? light.rgb + EmitterCausticsAt(gFragmentPixel) : -1.0;
}

// the emitters' highlights on the main pass's opaque fragments where the glossy rays don't take them (see Shade): the
// points the four traced pixels around the fragment's found reaching them this frame (EmitterPlacePoint), each weighed
// by the fragment's specular lobe toward it (by its traced pixel's contribution weight: an estimate of the light over
// the points that pixel could pick, which a fragment of the same surface can be lit by), the traced pixels by where the
// fragment is between them and how much theirs is its surface (as EmitterUpsample's). 0 where none is
float3 EmitterSpecular(Surface surface, float3 view, float3 position)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	uint2 extent = EmitterTracedExtent();
	uint tracedCount = extent.x * extent.y;
	Texture2D surfaces = gTextures[SHADER_TYPES_EMITTER_SURFACE_TEXTURE + (data.frame & 1u)];
	float3 eye = gViewData[gPushConstants.frameIndex][gShadingViewId].eyePosition.xyz;
	float scale = max(0.01 * distance(position, eye), 1e-6);
	float nearest = 0.002 * distance(position, eye);
	Surface specular = surface;
	specular.diffuseColor = 0.0;
	float2 at = float2(gFragmentPixel) / float(data.stride);
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
		float4 seen = surfaces.Load(int3(tap, 0));
		if (seen.w < 0.0)
			continue;
		float similar = exp(-abs(dot(seen.xyz - position, surface.geometryNormal)) / scale) *
						pow(saturate(dot(EmitterUnpackNormal(seen.w), surface.geometryNormal)), 8.0);
		float w = (offset.x != 0 ? f.x : 1.0 - f.x) * (offset.y != 0 ? f.y : 1.0 - f.y) * similar;
		weights += w;
		uint index = uint(tap.y) * extent.x + uint(tap.x);
		uint4 light = gEmitterReservoirs[(kEmitterPointsPart + 1u) * tracedCount + index];
		if (light.w == 0u)
			continue;
		uint4 placed = gEmitterReservoirs[kEmitterPointsPart * tracedCount + index];
		EmitterPoint point;
		point.position = asfloat(placed.xyz);
		point.normal = EmitterUnpackNormal(asfloat(placed.w));
		point.emission = asfloat(light.xyz);
		point.area = 0.0;
		point.doubleSided = light.w == 2u;
		float3 toPoint;
		float distance;
		float geometry = EmitterGeometry(position, surface.normal, point, nearest * nearest, toPoint, distance);
		if (geometry <= 0.0)
			continue;
		total += w * BaseLobe(specular, view, toPoint, dot(surface.normal, toPoint)) * point.emission * geometry;
	}
	return weights > 1e-4 ? total / weights : 0.0;
}

// the visibility buffer's emitter highlights (EmitterSpecular's, now; known: whether they were taken at the pixel) over
// frames: blended into the last frame's at where the pixel's point was then, which Emitter Temporal gathered into the
// frame's image with the diffuse's, by its reprojection, as Emitter Temporal blends the diffuse's, the frame's kept there
// for the next (SHADER_TYPES_EMITTER_SPECULAR_TEXTURE, -1 where not known). a frame's alone, four points every stride of
// pixels each way weighed by each pixel's lobe, was a grain over rough metal (veach-mis's roughest plate, its frames 600
// and 601 apart by 0.27 of 255, 0.11 before); the reprojection here rather than in Emitter Temporal cost Bistro at night
// 0.26 ms a frame more at 1440p. in compute only (see TracedReflectionRecord); a fragment's are the frame's
float3 EmitterSpecularOverFrames(float3 now, bool known)
{
	__stage_switch
	{
	case compute:
	{
		EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
		RWTexture2D history = gRWTextures[SHADER_TYPES_EMITTER_SPECULAR_RW_TEXTURE + (data.frame & 1u)];
		if (!known)
		{
			if (EmittersLight())
				history[gFragmentPixel] = float4(0.0, 0.0, 0.0, -1.0);
			return now;
		}
		ViewData view = gViewData[gPushConstants.frameIndex][gShadingViewId];
		float4 result = float4(now, 1.0);
		float4 kept = history[gFragmentPixel];
		if (kept.a > 0.0)
		{
			float blend = data.raw == 1u ? 1.0 : data.blend;
			float least = data.raw == 0u && EmitterViewStill(view) ? data.stillBlend : blend;
			float count = min(kept.a + 1.0, 1.0 / least);
			float3 sample = count > kEmitterFireflyFrameCount && data.raw == 0u ? HistoryFireflyClamp(now, kept.rgb, view.exposure) : now;
			result = float4(lerp(kept.rgb, sample, saturate(max(1.0 / count, least))), count);
		}
		history[gFragmentPixel] = result;
		return result.rgb;
	}
	default:
		return now;
	}
}

// the emitters' caustics: the light the emissive surfaces send through solid glass that gathers it (see
// TransmissionSolid; which their shadow rays stop at while these are traced, EmitterGatheredByGlass), as photons into
// cells of the world where they land (EmitterCausticCellsAdd), which hold it over frames and the frame's pixels and the
// traced hits take (EmitterCausticCellLight). the shadow views' photons
// (ComputeCaustics) are the punctual lights' and the sun's alone, landing in their views' tiles of the shadow
// transmittance, of which an emitter has none: cornell-caustic's water (the glTF research set's: a small emissive quad
// over a box filled up to two thirds with water) cast none, and the floor and walls under it were lit straight through
// the waves, a flat light where the reference's is a net of bright lines (docs/research-scenes-plan.md, C3)

// a triangle by its share of the emitters' power (as the tree's nodes bound it, see EmitterNode): down from the root,
// each child by its power's share of the two; and the probability it was picked with (0 for none)
uint EmitterPickByPower(inout EmitterRandom random, out float probability)
{
	probability = 1.0;
	uint node = 0;
	for (uint depthIt = 0; depthIt < EMITTER_NODE_DEPTH_MOST; depthIt++)
	{
		uint bits = asuint(gEmitterTree[node].axis.w);
		if ((bits & EMITTER_NODE_LEAF) != 0u)
			return bits & EMITTER_NODE_INDEX_MASK;
		uint second = bits & EMITTER_NODE_INDEX_MASK;
		float first = gEmitterTree[node + 1u].low.w;
		float other = gEmitterTree[second].low.w;
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

// whether a point is in one of the frame's views: within its viewport and in front of its eye, whatever its pixels show
bool EmitterCausticInView(float3 position)
{
	uint frameIndex = gPushConstants.frameIndex;
	for (uint viewIt = 0; viewIt < gPushConstants.viewCount; viewIt++)
	{
		float4 clip = mul(gViewData[frameIndex][viewIt].viewProjection, float4(position, 1.0));
		if (clip.w > 0.0 && all(abs(clip.xy) <= clip.w))
			return true;
	}
	return false;
}

// the Emitter Caustics pass: EmitterData::causticPhotonCount photons, each from a point of an emitter triangle picked by
// its power (EmitterPickByPower; a point of it uniformly), toward the glass (uniformly within the cone of the sphere about
// it, EmitterData::glass, from outside it; cosine weighted about the emitter's facing from inside it), each the light of
// its share of the emitters' (their radiance by the emission's cosine over the densities it was picked with), through the
// transmissive surfaces as the shadow views' photons go (GlassPathStep, absorbing along the path as the traced view's:
// the set's solids by its conditions have no thickness to absorb by), until it meets an opaque surface, where it lands
// in the cells of the world there (EmitterCausticCellsAdd, where a view holds the place) if solid glass gathered it (EmitterGatheredByGlass's rule, by the faces its own path met) or
// reflected it: met first, through thin walls, out of the body its emitter is inside, through slabs or into a flat face,
// it is the light the emitters' shadow rays give, which they let through. a photon that starts inside a solid's volume (a
// filament in a bulb taken as solid) leaves it, as the traced reflections' paths do
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeEmitterCaustics(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint frameIndex = gPushConstants.frameIndex;
	EmitterData data = gEmitterData[frameIndex][0];
	uint photon = dispatchThreadId.x;
	if (photon >= data.causticPhotonCount)
		return;
	EmitterRandom random = EmitterRandomAt(uint2(photon, 0x632be5abu), data.frame);
	float probability;
	uint triangle = EmitterPickByPower(random, probability);
	float2 barycentrics = EmitterBarycentrics(random);
	if (triangle == ~0u || !(probability > 0.0))
		return;
	EmitterPoint point = EmitterPointAt(triangle, barycentrics);
	if (!any(point.emission > 0.0))
		return;
	float3 direction;
	float density; // per solid angle
	float3 t;
	float3 b;
	float3 toGlass = data.glass.xyz - point.position;
	float distance2 = dot(toGlass, toGlass);
	float radius2 = data.glass.w * data.glass.w;
	if (distance2 > radius2)
	{
		float within = radius2 / distance2;
		float span = within / (1.0 + sqrt(1.0 - within)); // 1 less the cosine of the cone's half angle, unrounded
		float cosine = 1.0 - random.Next() * span;
		float sine = sqrt(max(1.0 - cosine * cosine, 0.0));
		float angle = random.Next() * 6.28318530718;
		float3 axis = toGlass * rsqrt(distance2);
		ShadowBasis(axis, t, b);
		direction = axis * cosine + (t * cos(angle) + b * sin(angle)) * sine;
		density = 1.0 / (6.28318530718 * span);
	}
	else
	{
		float3 facing = point.normal;
		float sides = 1.0;
		if (point.doubleSided)
		{
			sides = 0.5;
			if (random.Next() < 0.5)
				facing = -facing;
		}
		float u = random.Next();
		float angle = random.Next() * 6.28318530718;
		float cosine = sqrt(1.0 - u);
		float sine = sqrt(u);
		ShadowBasis(facing, t, b);
		direction = facing * cosine + (t * cos(angle) + b * sin(angle)) * sine;
		density = sides * cosine / kPi;
	}
	float emitted = dot(point.normal, direction);
	if (point.doubleSided)
		emitted = abs(emitted);
	if (!(emitted > 0.0) || !(density > 0.0))
		return;
	float eps = max(1e-4, 1e-5 * max(abs(point.position.x), max(abs(point.position.y), abs(point.position.z))));
	GlassPath path;
	path.origin = point.position + point.normal * (dot(point.normal, direction) > 0.0 ? eps : -eps);
	path.direction = direction;
	path.throughput = point.emission * (emitted * point.area / (probability * density * float(data.causticPhotonCount)));
	path.inside = false;
	path.attenuationColor = 1.0;
	path.attenuationDistance = 0.0;
	bool gathered = false; // whether solid glass gathered it (see EmitterGatheredByGlass, whose rule this follows)
	bool reflected = false; // or it was reflected off glass
	bool entered = false; // in a solid it went into: by the face it went in by
	float3 entryNormal = 0.0;
	bool entryFlat = false;
	float3 entryPosition = 0.0;
	float entryReach = 0.0;
	for (uint surfaceIt = 0; surfaceIt < GlassSurfaceCount(); surfaceIt++)
	{
		bool glass;
		BvhHit hit = BvhTraceSceneOrGlass(path.origin, path.direction, 1e30, GiAlphaMask(), glass);
		if (!BvhHitSomething(hit))
			return;
		if (!glass)
		{
			// (landing inside the solid it went into: gathered unless by a flat face)
			gathered = gathered || (entered && !entryFlat);
			if (gathered || reflected)
			{
				if (path.inside)
					path.throughput *= VolumeAttenuation(path.attenuationColor, path.attenuationDistance, hit.t);
				float3 landing = path.origin + path.direction * hit.t;
				if (EmitterCausticInView(landing))
					EmitterCausticCellsAdd(landing, hit.normal, path.throughput, random);
			}
			return;
		}
		GlassSurface surface = GlassSurfaceAt(path.origin, path.direction, hit, frameIndex);
		bool solidHere = TransmissionSolid(surface.material.transmission.y);
		if (surfaceIt == 0u && solidHere && !hit.frontFace)
		{
			path.inside = true;
			path.attenuationColor = surface.material.attenuationColor.rgb;
			path.attenuationDistance = surface.material.transmission.z;
		}
		// at a surface it meets from outside, reflected as often as the surface's Fresnel reflection says (Russian
		// roulette: its light as it was), else on through it, its light the more by the chance (GlassPathStep takes the
		// reflection's share off). the light glass reflects is no one else's: the shadow rays go straight, and the
		// indirect lighting's rays don't meet glass. cornell-caustic's ceiling, lit by what its water's waves reflect up
		// in the reference, took only the indirect light. a flat face's reflection is dropped: it mirrors the emitter,
		// gathering nothing, a few photons of it a speck each on the walls around modern-hall's rail
		if (!path.inside)
		{
			float fresnel = FresnelSchlick(float3(surface.f0), 1.0, saturate(dot(-path.direction, surface.normal))).r;
			if (random.Next() < fresnel)
			{
				if (surface.flat)
					return;
				path.direction = reflect(path.direction, surface.normal);
				path.origin = surface.position + surface.normal * surface.eps;
				reflected = true;
				continue;
			}
			path.throughput /= max(1.0 - fresnel, 1e-6);
		}
		bool wasInside = path.inside;
		if (solidHere)
		{
			if (!wasInside)
			{
				entered = true;
				entryNormal = surface.normal;
				entryFlat = surface.flat;
				entryPosition = surface.position;
				entryReach = kEmitterShellReach * surface.radius;
			}
			// out of the solid it went into: gathered unless it was a slab there (a shell's other face near, see
			// kEmitterShellReach), or flat where it went in and here; a body inside the body gathers
			else if (entered)
			{
				bool slab = dot(entryNormal, surface.normal) >= kEmitterSlabCosine && (entryFlat || distance(entryPosition, surface.position) <= entryReach);
				if (hit.frontFace || !(slab || (entryFlat && surface.flat)))
					gathered = true;
			}
		}
		if (!GlassPathStep(path, surface, frameIndex, true))
			return;
		if (solidHere && wasInside)
		{
			if (path.inside) // reflected back inside: gathered unless off a flat face
				gathered = gathered || (entered && !surface.flat);
			else
				entered = false;
		}
	}
}

// the emitters' caustics' light at a pixel (the Emitter Caustic Light pass's, from the photons' cells): the irradiance
// over pi, 0 where they aren't traced
float3 EmitterCausticsAt(int2 pixel)
{
	if (!EmitterCausticsTraced())
		return 0.0;
	return gTextures[SHADER_TYPES_EMITTER_CAUSTIC_TEXTURE].Load(int3(pixel, 0)).rgb;
}

// the photons' cells of the world (gEmitterCausticCells, EmitterData::causticCellCount of them): the emitters' caustics'
// light where the photons land, which the frame's pixels (Emitter Caustic Light) and the traced hits the screen hides
// (TracedEmitterHidden) both take. the caustics are the world's, the same from wherever they are seen while the glass and
// the emitters are still, so they are kept in the world: a hash table as the world cache's (GiWorldMix), a cell a cube of
// a power of two wide (its level) of a normal bucket (GiWorldBucket: the two sides of a wall cells apart), found by linear
// probing, the stalest of those probed taken back where none is empty (a policy, not a capacity: see
// kGiWorldProbeCount). a photon lands in the cell about kEmitterCausticCellPixels pixels wide at its distance from the
// first view's eye and in the two levels coarser (EmitterCausticCellsAdd), wherever in a view it lands; a cell holds the
// sum of its photons' light since the frame it was made in (in EmitterData::causticUnit, fixed point, rounded at
// random), its photons' count and its area of the surface's plane; a lookup (EmitterCausticCellLight) takes the fine
// level's light where its cells have photons enough and the coarse one's where they don't, each interpolated over the
// 8 cells around the point.
// before, the photons landed on the frame's pixels (a buffer of sums a pixel, emptied each frame) and were averaged over
// frames with the emitters' light, in its history: 256 frames while the view was still and 32 once it moved, a few
// photons a pixel, a grain that began again with every move of the camera (the user's report, 2026-11-23: "settle
// slowly"), and cells only where the screen hid a photon's landing, 2 pixels wide and of the same 256 frames, mottled
// ("unnatural behind objects after moving the camera": docs/research-scenes-review-plan.md, report 4)

// a cell's words: its key (0: empty), the frame a photon last landed in it (EmitterData::frame), the frame its sums
// begin after, its photons' count, their sums (3 words) and its area of its surface's plane (a float, 0 until known)
static const uint kEmitterCausticCellKey = 0u;
static const uint kEmitterCausticCellStamp = 1u;
static const uint kEmitterCausticCellBirth = 2u;
static const uint kEmitterCausticCellCount = 3u;
static const uint kEmitterCausticCellSums = 4u;
static const uint kEmitterCausticCellArea = 7u;
static const uint kEmitterCausticCellProbeCount = 16u;
// the frames since a cell was last used (a photon landed in it, or a pixel took its light) from which it may be taken
// back for another place: sooner, where the table is full around it the cells took each other's places every frame
static const uint kEmitterCausticCellStaleFrameCount = 64u;
// the frames a cell's light is of, and the photons, from either of which it counts in full (see
// EmitterCausticLevelLight)
static const float kEmitterCausticCellSettledFrameCount = 32.0;
static const float kEmitterCausticCellSettledPhotonCount = 16.0;
// the finest cells' side, in pixels at their distance (to a power of two: 1.4 to 2.8 of them)
static const float kEmitterCausticCellPixels = 2.0;
// the levels a photon lands in: its place's and those coarser, each twice as wide
static const int kEmitterCausticCellLevelCount = 3;
// the photons a level's cells' light counts from over the level above's, and from which it counts alone
static const float kEmitterCausticCellFewPhotons = 8.0;
static const float kEmitterCausticCellEnoughPhotons = 48.0;
// the grid's offset, in sides: off whole numbers, where models' walls lie (cornell-caustic's at 1 and -1), so that a wall's
// photons fall in one layer of cells, not in two by their rounding
static const float kEmitterCausticCellOffset = 0.3819660113;

uint EmitterCausticCellWord(uint cell, uint word)
{
	return cell * SHADER_TYPES_EMITTER_CAUSTIC_CELL_WORD_COUNT + word;
}

// the level of the finest cells of a point of a surface (their side's log2; normal: the surface's): the cell that
// covers kEmitterCausticCellPixels squared of the first view's pixels there, the wider the more the surface slants from
// the eye (a cell is as wide along the slant as across). by the distance alone, a floor seen at a low angle was several
// cells to a pixel, more than the table holds: its cells were taken back for others' all the time and began again, each
// a dark dot among its neighbours' light
int EmitterCausticCellLevel(float3 position, float3 normal)
{
	ViewData view = gViewData[gPushConstants.frameIndex][0];
	float pixel = length(ViewRay(view, float2(0.0, 0.0)) - ViewRay(view, float2(2.0 / view.viewport.z, 0.0)));
	float3 toEye = view.eyePosition.xyz - position;
	float eye = max(length(toEye), 1e-30);
	float slant = max(abs(dot(normal, toEye / eye)), 0.0625);
	float wanted = max(eye * pixel * kEmitterCausticCellPixels * rsqrt(slant), 1e-30);
	return clamp(int(round(log2(wanted))), -120, 120);
}

int3 EmitterCausticCellPlace(float3 position, float side)
{
	return int3(floor(position / side + kEmitterCausticCellOffset));
}

// where a cell's probe begins and the key it is told apart by (two hashes independent of each other, see GiWorldHash)
uint EmitterCausticCellHash(int3 place, int level, uint bucket)
{
	return GiWorldMix(uint(place.x) * 73856093u ^ GiWorldMix(uint(place.y) * 19349663u ^ GiWorldMix(uint(place.z) * 83492791u ^ (uint(level + 128) << 3) ^ bucket)));
}

uint EmitterCausticCellKey(int3 place, int level, uint bucket)
{
	return GiWorldMix(uint(place.z) * 0x632BE59Bu ^ GiWorldMix(uint(place.x) * 0x85157AF5u ^ GiWorldMix(uint(place.y) * 0x9E3779B1u ^ (uint(level + 128) << 3) ^ bucket ^ 0xA511E9B3u))) | 1u;
}

// a cell's index, ~0 where there is none: found by its key along the probe from its hash, or (insert) made in the first
// empty one, else in place of the stalest probed that nothing has used for kEmitterCausticCellStaleFrameCount frames
// (emptied: its light another place's; none so stale, and the photon's light isn't kept at that level: a lookup takes
// the level above there). made: this call made it
uint EmitterCausticCellFind(int3 place, int level, uint bucket, bool insert, out bool made)
{
	made = false;
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	uint frame = data.frame;
	uint mask = data.causticCellCount - 1u;
	uint hash = EmitterCausticCellHash(place, level, bucket);
	uint key = EmitterCausticCellKey(place, level, bucket);
	uint stalest = ~0u;
	uint stalestAge = 0u;
	uint stalestStamp = 0u;
	for (uint probeIt = 0; probeIt < kEmitterCausticCellProbeCount; probeIt++)
	{
		uint index = (hash + probeIt) & mask;
		uint keyWord = EmitterCausticCellWord(index, kEmitterCausticCellKey);
		uint found = gEmitterCausticCells[keyWord];
		if (found == key)
			return index;
		if (found == 0u)
		{
			if (!insert)
				return ~0u;
			uint previous;
			InterlockedCompareExchange(gEmitterCausticCells[keyWord], 0u, key, previous);
			made = previous == 0u;
			if (previous == 0u || previous == key)
				return index;
			continue;
		}
		// (a stamp of 0: a cell just made, its maker yet to stamp it)
		uint stamp = gEmitterCausticCells[EmitterCausticCellWord(index, kEmitterCausticCellStamp)];
		uint age = frame - stamp;
		if (stamp != 0u && age > stalestAge && age > kEmitterCausticCellStaleFrameCount)
		{
			stalestAge = age;
			stalest = index;
			stalestStamp = stamp;
		}
	}
	if (!insert || stalest == ~0u)
		return ~0u;
	// taken back by its stamp: the one thread that swaps the stale stamp it saw for the frame's has the cell, and nothing
	// takes a cell stamped this frame. by the key (whoever swapped the key it read then for its own), a thread took the
	// cell another had taken the moment before, which went on to write its area into it: cells with the area of another
	// level's, 16 times too dark or too bright, dark dots over cornell-caustic's walls and the whole 40% too bright
	uint previous;
	InterlockedCompareExchange(gEmitterCausticCells[EmitterCausticCellWord(stalest, kEmitterCausticCellStamp)], stalestStamp, frame, previous);
	if (previous != stalestStamp)
		return ~0u;
	gEmitterCausticCells[EmitterCausticCellWord(stalest, kEmitterCausticCellKey)] = key;
	for (uint wordIt = 2u; wordIt < SHADER_TYPES_EMITTER_CAUSTIC_CELL_WORD_COUNT; wordIt++)
		gEmitterCausticCells[EmitterCausticCellWord(stalest, wordIt)] = 0u;
	made = true;
	return stalest;
}

// a photon landing (at position, its normal toward where it came from; flux its light) added to its cell of a level. the
// photon that makes a cell gives it its area (of the surface's plane where it landed, at least a tenth of the cell's
// side squared: a cell the plane barely cuts would take its few photons as very bright) and the frame its sums begin
// after, and adds nothing, nor do the frame's others: a cell is made because a photon landed, and with that photon its
// light would be the more the younger it is, a speck wherever one is made where few land. what it adds is its light over
// the cell's side squared (the lookup takes the area's share), so that the sums are of the frames' own scale
void EmitterCausticCellAdd(float3 position, float3 normal, float3 flux, int level, inout EmitterRandom random)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	float side = exp2(float(level));
	int3 place = EmitterCausticCellPlace(position, side);
	bool made;
	uint cell = EmitterCausticCellFind(place, level, GiWorldBucket(normal), true, made);
	if (cell == ~0u)
		return;
	gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellStamp)] = data.frame;
	if (made)
	{
		float3 low = (float3(place) - kEmitterCausticCellOffset) * side;
		float area = max(EmitterPlaneBoxArea(low, low + side, position, normal), 0.1 * side * side);
		gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellArea)] = asuint(area);
		gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellBirth)] = data.frame;
		return;
	}
	if (gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellBirth)] == data.frame)
		return;
	float3 amount = flux * (data.causticUnit / (kPi * side * side));
	for (uint channel = 0; channel < 3; channel++)
	{
		uint units = uint(min(amount[channel] + random.Next(), 1048576.0));
		if (units > 0u)
			InterlockedAdd(gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellSums + channel)], units);
	}
	InterlockedAdd(gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellCount)], 1u);
}

// and to its cells of every level it lands in (kEmitterCausticCellLevelCount: the fine level's and those coarser, of
// which a lookup takes each where the finer one's photons are few, see EmitterCausticCellLight)
void EmitterCausticCellsAdd(float3 position, float3 normal, float3 flux, inout EmitterRandom random)
{
	int level = EmitterCausticCellLevel(position, normal);
	for (int levelIt = 0; levelIt < kEmitterCausticCellLevelCount; levelIt++)
		EmitterCausticCellAdd(position, normal, flux, level + levelIt, random);
}

// the Emitter Caustic Cells pass, a thread per cell of the frame's share of the table (1 in
// SHADER_TYPES_EMITTER_CAUSTIC_CELL_SLICE_COUNT, each cell once in that many frames): a cell whose sums are of more
// frames than EmitterData::causticFrameCap, or near what a word holds, is halved, its sums, its photons and its frames
// (its light the same, of half as many frames: what changes the caustics shows within the cap, as the light and the
// glass do; the view doesn't change them)
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeEmitterCausticCells(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	uint slice = data.causticCellCount / SHADER_TYPES_EMITTER_CAUSTIC_CELL_SLICE_COUNT;
	if (dispatchThreadId.x >= slice)
		return;
	uint cell = (data.frame % SHADER_TYPES_EMITTER_CAUSTIC_CELL_SLICE_COUNT) * slice + dispatchThreadId.x;
	if (gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellKey)] == 0u)
		return;
	uint birthWord = EmitterCausticCellWord(cell, kEmitterCausticCellBirth);
	uint age = data.frame - gEmitterCausticCells[birthWord];
	uint3 sums;
	for (uint channel = 0; channel < 3; channel++)
		sums[channel] = gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellSums + channel)];
	if (age <= data.causticFrameCap && all(sums < 0x40000000u))
		return;
	for (uint channel = 0; channel < 3; channel++)
		gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellSums + channel)] = sums[channel] >> 1u;
	uint countWord = EmitterCausticCellWord(cell, kEmitterCausticCellCount);
	gEmitterCausticCells[countWord] = gEmitterCausticCells[countWord] >> 1u;
	gEmitterCausticCells[birthWord] = data.frame - (age >> 1u);
}

// a box's corner (index: its bits x, y and z, the high side's where set)
float3 EmitterBoxCorner(float3 low, float3 high, uint index)
{
	return float3((index & 1u) != 0u ? high.x : low.x, (index & 2u) != 0u ? high.y : low.y, (index & 4u) != 0u ? high.z : low.z);
}

// the area of a plane (through point, of unit normal) inside a box (low to high): the polygon the box's edges cut it to
// (3 to 6 corners), about their mean in order of their angle
float EmitterPlaneBoxArea(float3 low, float3 high, float3 point, float3 normal)
{
	float distances[8];
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		distances[cornerIt] = dot(EmitterBoxCorner(low, high, cornerIt) - point, normal);
	float3 points[6];
	uint count = 0;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		for (uint axisIt = 0; axisIt < 3; axisIt++)
		{
			uint other = cornerIt | (1u << axisIt);
			if (other == cornerIt || (distances[cornerIt] < 0.0) == (distances[other] < 0.0) || count >= 6u)
				continue;
			points[count] = lerp(
				EmitterBoxCorner(low, high, cornerIt), EmitterBoxCorner(low, high, other), distances[cornerIt] / (distances[cornerIt] - distances[other]));
			count++;
		}
	if (count < 3u)
		return 0.0;
	float3 center = 0.0;
	for (uint pointIt = 0; pointIt < count; pointIt++)
		center += points[pointIt];
	center /= float(count);
	float3 t;
	float3 b;
	ShadowBasis(normal, t, b);
	float angles[6];
	for (uint pointIt = 0; pointIt < count; pointIt++)
		angles[pointIt] = atan2(dot(points[pointIt] - center, b), dot(points[pointIt] - center, t));
	for (uint pointIt = 1; pointIt < count; pointIt++)
		for (uint backIt = pointIt; backIt > 0u && angles[backIt - 1u] > angles[backIt]; backIt--)
		{
			float angle = angles[backIt];
			angles[backIt] = angles[backIt - 1u];
			angles[backIt - 1u] = angle;
			float3 at = points[backIt];
			points[backIt] = points[backIt - 1u];
			points[backIt - 1u] = at;
		}
	float area = 0.0;
	for (uint pointIt = 0; pointIt < count; pointIt++)
		area += dot(cross(points[pointIt] - center, points[(pointIt + 1u) % count] - center), normal);
	return 0.5 * abs(area);
}

// a level's light at a point (of a normal bucket): the irradiance over pi its cells hold, interpolated over the 8 around
// the point (trilinear), those not there or made this frame left out (nothing has landed there, or nothing is there: a
// surface's cells are a layer among empty ones), and a young cell counting by how settled it is (its frames of
// kEmitterCausticCellSettledFrameCount, or its photons of kEmitterCausticCellSettledPhotonCount: its mean is of few
// frames, none of its light in most and many frames' worth in one, a dark dot or a bright one among settled cells); the
// weights of those taken (0: none) and the photons they hold there. touch: the cells it takes are stamped as used (the
// Emitter Caustic Light pass's: what the frame shows isn't taken back for another place)
float3 EmitterCausticLevelLight(float3 position, uint bucket, int level, bool touch, out float weights, out float photons)
{
	EmitterData data = gEmitterData[gPushConstants.frameIndex][0];
	float side = exp2(float(level));
	float3 at = position / side + kEmitterCausticCellOffset - 0.5;
	int3 base = int3(floor(at));
	float3 f = at - float3(base);
	float3 total = 0.0;
	weights = 0.0;
	photons = 0.0;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		int3 offset = int3(cornerIt & 1u, (cornerIt >> 1u) & 1u, cornerIt >> 2u);
		bool made;
		uint cell = EmitterCausticCellFind(base + offset, level, bucket, false, made);
		if (cell == ~0u)
			continue;
		uint age = data.frame - gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellBirth)];
		float area = asfloat(gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellArea)]);
		if (age == 0u || !(area > 0.0))
			continue;
		if (touch)
		{
			uint stampWord = EmitterCausticCellWord(cell, kEmitterCausticCellStamp);
			if (gEmitterCausticCells[stampWord] != data.frame)
				gEmitterCausticCells[stampWord] = data.frame;
		}
		float count = float(gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellCount)]);
		float w = (offset.x != 0 ? f.x : 1.0 - f.x) * (offset.y != 0 ? f.y : 1.0 - f.y) * (offset.z != 0 ? f.z : 1.0 - f.z);
		w *= saturate(max(float(age) / kEmitterCausticCellSettledFrameCount, count / kEmitterCausticCellSettledPhotonCount));
		float3 sums = 0.0;
		for (uint channel = 0; channel < 3; channel++)
			sums[channel] = float(gEmitterCausticCells[EmitterCausticCellWord(cell, kEmitterCausticCellSums + channel)]);
		total += sums * (w * side * side / (area * float(age)));
		weights += w;
		photons += w * count;
	}
	if (weights < 1e-4)
	{
		weights = 0.0;
		photons = 0.0;
		return 0.0;
	}
	photons /= weights;
	return total / (weights * max(data.causticUnit, 1e-30));
}

// the emitters' caustics' light at a point (normal: its surface's, toward where it is seen from) from the photons' cells:
// the coarsest level's (kEmitterCausticCellLevelCount - 1 up: a cell's light there is of 16 times the fine one's photons,
// smooth from its first frames), and each finer level's over it by the photons its cells hold there (none below
// kEmitterCausticCellFewPhotons, all from kEmitterCausticCellEnoughPhotons: where the fine one's is still a grain, the
// one above shows, and the picture sharpens level by level as the photons come). a level with no cells there is passed
// by: the eye a level nearer or further since the photons landed, the levels they are in are still two of the three (and
// where the two coarsest have none, nothing has landed: the finest isn't looked for). 0 where none is. finest: the
// finest level taken, 0 for the point's own (the frame's pixels), 1 for the one above (a traced hit the screen hides,
// seen through glass or in a mirror: its three levels cost cornell-caustic's Main Transmissive 0.55 ms a frame at 1440p
// over two). the irradiance over pi
float3 EmitterCausticCellLight(float3 position, float3 normal, bool touch, int finest)
{
	uint bucket = GiWorldBucket(normal);
	int level = EmitterCausticCellLevel(position, normal);
	float3 light = 0.0;
	bool any = false;
	int missed = 0;
	for (int levelIt = kEmitterCausticCellLevelCount - 1; levelIt >= finest; levelIt--)
	{
		float weights;
		float photons;
		float3 here = EmitterCausticLevelLight(position, bucket, level + levelIt, touch, weights, photons);
		if (weights <= 0.0)
		{
			missed++;
			if (missed >= 2 && !any)
				break;
			continue;
		}
		light = any ? lerp(light, here, saturate((photons - kEmitterCausticCellFewPhotons) / (kEmitterCausticCellEnoughPhotons - kEmitterCausticCellFewPhotons)))
					: here;
		any = true;
	}
	return light;
}

// the Emitter Caustic Light pass: per pixel of the pre-pass's depth, the caustics' light at its point from the photons'
// cells (EmitterCausticCellLight), into SHADER_TYPES_EMITTER_CAUSTIC_TEXTURE, which the readers of the emitters' light
// add to it (EmitterCausticsAt). not through the emitters' history: the cells are the mean over frames, and they are
// where they were when the view moves
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeEmitterCausticLight(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D light = gRWTextures[SHADER_TYPES_EMITTER_CAUSTIC_RW_TEXTURE];
	uint2 extent;
	light.GetDimensions(extent.x, extent.y);
	if (any(dispatchThreadId.xy >= extent))
		return;
	int2 pixel = int2(dispatchThreadId.xy);
	ViewData view;
	ShadowSurface surface;
	float3 caustics = 0.0;
	if (ViewAt(pixel, view) && ShadowSurfaceAt(pixel, view, surface))
		caustics = min(EmitterCausticCellLight(surface.position, surface.normal, true, 0), 65000.0);
	light[pixel] = float4(caustics, 1.0);
}
