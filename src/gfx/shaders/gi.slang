// indirect lighting: world space irradiance probes over a voxel radiance volume (see GiData). a part of shaders.slang,
// which includes it after capi.h and sky.h

// a kernel's thread, of its rows of SHADER_TYPES_GI_ROW (see Renderer's giKernel)
uint3 GiThread(uint3 dispatchId)
{
	return uint3(dispatchId.y * SHADER_TYPES_GI_ROW + dispatchId.x, 0, 0);
}

// packing
uint GiPackHalf2(float2 v)
{
	return f32tof16(v.x) | (f32tof16(v.y) << 16);
}

float2 GiUnpackHalf2(uint v)
{
	return float2(f16tof32(v & 0xFFFFu), f16tof32(v >> 16));
}

uint2 GiPackRgba16(float4 v)
{
	return uint2(GiPackHalf2(v.xy), GiPackHalf2(v.zw));
}

float4 GiUnpackRgba16(uint2 v)
{
	return float4(GiUnpackHalf2(v.x), GiUnpackHalf2(v.y));
}

float2 GiOctahedral(float3 n)
{
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	float2 o = n.xy;
	if (n.z < 0.0)
		o = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, float2(1.0), float2(-1.0));
	return o;
}

float3 GiFromOctahedral(float2 o)
{
	float3 n = float3(o, 1.0 - abs(o.x) - abs(o.y));
	if (n.z < 0.0)
		n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, float2(1.0), float2(-1.0));
	return normalize(n);
}

// the direction of an octahedral map's texel (of side texels)
float3 GiDirection(uint2 texel, uint side)
{
	return GiFromOctahedral((float2(texel) + 0.5) / float(side) * 2.0 - 1.0);
}

// the solid angle of a spherical triangle (Van Oosterom and Strackee)
float GiTriangleSolidAngle(float3 a, float3 b, float3 c)
{
	float numerator = abs(dot(a, cross(b, c)));
	float denominator = 1.0 + dot(a, b) + dot(b, c) + dot(c, a);
	return 2.0 * atan2(numerator, denominator);
}

// the solid angle an octahedral map's texel covers (they aren't equal: by its corners, as two spherical triangles, each
// side of the map's fold apart)
float GiTexelSolidAngle(uint2 texel, uint side)
{
	float2 low = float2(texel) / float(side) * 2.0 - 1.0;
	float2 high = float2(texel + 1) / float(side) * 2.0 - 1.0;
	float3 c00 = GiFromOctahedral(low);
	float3 c10 = GiFromOctahedral(float2(high.x, low.y));
	float3 c01 = GiFromOctahedral(float2(low.x, high.y));
	float3 c11 = GiFromOctahedral(high);
	return GiTriangleSolidAngle(c00, c10, c11) + GiTriangleSolidAngle(c00, c11, c01);
}

// the volume
// a level's voxels per axis: rounded up, so that every level covers all of the volume. rounded down, a level ended up
// to 2^level - 1 voxels short of the volume's far side (Sponza's 61 voxels deep: level 4 covered 48), where rays
// left for the next clip early: as early as the box around the eye ended (see GiMarch) on some snaps of a clip and not
// on others, so its light switched between two values as it moved
uint3 GiLevelVoxels(uint3 size, uint level)
{
	return max((size + (1u << level) - 1u) >> level, uint3(1));
}

uint3 GiLevelSize(GiData gi, uint level)
{
	return GiLevelVoxels(gi.volumeSize.xyz, level);
}

uint GiLevelFirst(GiData gi, uint level)
{
	return gi.levelFirst[level / 4][level % 4];
}

uint GiVoxelIndex(uint3 voxel, uint3 size)
{
	return (voxel.z * size.y + voxel.y) * size.x + voxel.x;
}

// where a point is in a level's voxels (unclamped)
float3 GiVoxelCoordinate(GiData gi, float3 position, uint level)
{
	return (position - gi.volumeOrigin.xyz) / (gi.volumeOrigin.w * float(1u << level));
}

bool GiInside(int3 voxel, uint3 size)
{
	return all(voxel >= 0) && all(voxel < int3(size));
}

// a clip of the clipmap (see SHADER_TYPES_GI_CLIP_COUNT)
GiData GiClip(uint clip)
{
	return gGi[gPushConstants.frameIndex][clip];
}

// what loops over the clips read of one (the shadow rays, the shading's lookups): only these fields of its GiData,
// rather than a copy of it all, which cost the main pass and the voxels' lighting
struct GiClipView
{
	float4 volumeOrigin;
	uint4 volumeSize;
	uint voxelFirst;
	uint clip;
	uint clipCount;
	float probeSpacing; // in voxels
	float4 fadeCenter; // see GiData::fadeCenter
	float4 fadeExtent;
};

GiClipView GiClipViewAt(uint clip)
{
	GiClipView view;
	view.volumeOrigin = gGi[gPushConstants.frameIndex][clip].volumeOrigin;
	view.volumeSize = gGi[gPushConstants.frameIndex][clip].volumeSize;
	view.voxelFirst = gGi[gPushConstants.frameIndex][clip].voxelFirst;
	view.clip = clip;
	view.clipCount = gGi[gPushConstants.frameIndex][clip].clipCount;
	view.probeSpacing = float(gGi[gPushConstants.frameIndex][clip].probeSize.w);
	view.fadeCenter = gGi[gPushConstants.frameIndex][clip].fadeCenter;
	view.fadeExtent = gGi[gPushConstants.frameIndex][clip].fadeExtent;
	return view;
}

// how much a clip lights a point, where the finer ones don't (see GiSampleAt): the last clip all of what it holds; one that
// follows the eye 1 in the box around the eye it always holds (GiData::fadeCenter), fading to 0 toward the box's edges, and
// at most as much as it holds the point (more than a probe spacing inside it, fading over 8 voxels: where it is
// clamped to the last clip's edges, the box needn't be inside it)
float GiClipWeight(GiClipView clip, float3 position)
{
	float3 coordinate = (position - clip.volumeOrigin.xyz) / clip.volumeOrigin.w;
	float3 size = float3(clip.volumeSize.xyz);
	float edge = min(min(coordinate.x, coordinate.y), min(coordinate.z, min(min(size.x - coordinate.x, size.y - coordinate.y), size.z - coordinate.z)));
	if (clip.clip + 1 >= clip.clipCount)
		return edge > 0.0 ? 1.0 : 0.0;
	float3 beyond = abs(position - clip.fadeCenter.xyz) - (clip.fadeExtent.xyz - clip.fadeExtent.w);
	float fade = 1.0 - smoothstep(0.0, clip.fadeExtent.w, max(beyond.x, max(beyond.y, beyond.z)));
	return min(fade, saturate((edge - clip.probeSpacing) / 8.0));
}

// a clip's voxel (level 0)
GiVoxel GiVoxelAt(GiData gi, uint3 voxel)
{
	return gGiVoxels[gi.voxelFirst + GiVoxelIndex(voxel, GiLevelSize(gi, 0))];
}

// a level's radiance (times occupancy) and occupancy at a voxel, 0 outside the volume
// a level's face of a voxel (see ComputeGiMip): the levels above 0 hold 6 per voxel, one per axis direction (face 2
// axis, + 1 for the negative direction), a face's voxels after the one before's
uint GiRadianceIndex(uint levelFirst, uint level, uint3 voxel, uint3 size, uint face)
{
	return levelFirst + (level == 0u ? 0u : face * size.x * size.y * size.z) + GiVoxelIndex(voxel, size);
}

float4 GiRadianceAt(GiData gi, uint level, int3 voxel, uint face)
{
	uint3 size = GiLevelSize(gi, level);
	if (!GiInside(voxel, size))
		return 0.0;
	return GiUnpackRgba16(gGiRadiance[GiRadianceIndex(GiLevelFirst(gi, level), level, uint3(voxel), size, face)]);
}

float4 GiRadianceAt(GiData gi, uint level, int3 voxel)
{
	uint3 size = GiLevelSize(gi, level);
	if (!GiInside(voxel, size))
		return 0.0;
	return GiUnpackRgba16(gGiRadiance[GiLevelFirst(gi, level) + GiVoxelIndex(uint3(voxel), size)]);
}

// how far along a ray from origin (world space) it stays in a clip's box around the eye (GiData::fadeCenter): the
// distance to where it leaves it, 0 if it starts outside, and the last clip's holds everything
float GiFadeBoxExit(float4 fadeCenter, float4 fadeExtent, float3 origin, float3 direction)
{
	float3 relative = origin - fadeCenter.xyz;
	if (any(abs(relative) > fadeExtent.xyz))
		return 0.0;
	float3 bound = select(direction >= 0.0, fadeExtent.xyz, -fadeExtent.xyz);
	float3 distances = (bound - relative) / select(abs(direction) > 1e-8, direction, 1e-8);
	return max(min(distances.x, min(distances.y, distances.z)), 0.0);
}

// along a ray through the volume, from start to end (world space distances): the radiance reaching its origin (front to
// back) and how much of what is beyond gets through. at level, or with cone at the level of its distance: level 0 as far
// as two probe spacings, each level above four times as far again (level 1 to 10 spacings, 2 to 42: a cone of a constant
// angle), as far as the clip has levels that stay put (GiData::marchLevelCount). where it leaves the clip it goes on
// through the coarser ones (never back: they are boxes), at their level of the same voxel size (or level 0), at half
// their voxels' steps. a clip that follows the eye is left where its box around the eye is (GiData::fadeCenter), inside
// its own, so that where a ray goes on through the coarser clip's voxels (thicker) moves with the eye, smoothly, rather
// than with the clip's snaps
float4 GiMarch(GiData gi, uint level, bool cone, float3 origin, float3 direction, float start, float end)
{
	float3 radiance = 0.0;
	float transmittance = 1.0;
	// what the steps read of the clip they are in, and its level: only these, rather than a copy of its GiData, read
	// field by field when the ray leaves for the next clip or changes level
	uint clip = gi.clip;
	uint clipCount = gi.clipCount;
	uint clipsCrossed = 0;
	uint maxLevel = min(gi.volumeSize.w, gi.marchLevelCount) - 1u;
	uint clipLevel = min(level, maxLevel);
	float4 volumeOrigin = gi.volumeOrigin;
	uint3 levelSize = GiLevelSize(gi, clipLevel);
	uint levelFirst = GiLevelFirst(gi, clipLevel);
	float voxelSize = volumeOrigin.w * float(1u << clipLevel);
	float spacing = gi.probeOrigin.w;
	float boxExit = GiFadeBoxExit(gi.fadeCenter, gi.fadeExtent, origin, direction);
	float t = start;
	while (t < end && transmittance > 0.01)
	{
		if (cone)
		{
			// the level of the distance, in the first clip's levels; this clip's is one less per clip crossed (its voxels
			// are twice the size)
			uint wanted = uint(floor(0.5 * log2(1.5 * t / spacing + 1.0)));
			uint next = uint(clamp(int(wanted) - int(clipsCrossed), 0, int(maxLevel)));
			if (next != clipLevel)
			{
				clipLevel = next;
				levelSize = GiLevelVoxels(gGi[gPushConstants.frameIndex][clip].volumeSize.xyz, clipLevel);
				levelFirst = gGi[gPushConstants.frameIndex][clip].levelFirst[clipLevel / 4][clipLevel % 4];
				voxelSize = volumeOrigin.w * float(1u << clipLevel);
			}
		}
		float step = voxelSize * 0.5;
		float3 position = origin + direction * (t + step * 0.5);
		int3 voxel = int3(floor((position - volumeOrigin.xyz) / voxelSize));
		if (!GiInside(voxel, levelSize) || t + step * 0.5 > boxExit)
		{
			if (clip + 1 >= clipCount)
				break;
			clip++;
			clipsCrossed++;
			boxExit = GiFadeBoxExit(gGi[gPushConstants.frameIndex][clip].fadeCenter, gGi[gPushConstants.frameIndex][clip].fadeExtent, origin, direction);
			volumeOrigin = gGi[gPushConstants.frameIndex][clip].volumeOrigin;
			uint4 volumeSize = gGi[gPushConstants.frameIndex][clip].volumeSize;
			maxLevel = min(volumeSize.w, gGi[gPushConstants.frameIndex][clip].marchLevelCount) - 1u;
			clipLevel = min(clipLevel > 0u ? clipLevel - 1u : 0u, maxLevel);
			levelSize = GiLevelVoxels(volumeSize.xyz, clipLevel);
			levelFirst = gGi[gPushConstants.frameIndex][clip].levelFirst[clipLevel / 4][clipLevel % 4];
			voxelSize = volumeOrigin.w * float(1u << clipLevel);
			continue;
		}
		// a coarse voxel as a ray along direction sees it: the faces it looks through, by the direction's squares
		float4 sample;
		if (clipLevel == 0u)
			sample = GiUnpackRgba16(gGiRadiance[levelFirst + GiVoxelIndex(uint3(voxel), levelSize)]);
		else
		{
			float3 weights = direction * direction;
			uint3 faces = uint3(0u, 2u, 4u) + select(direction < 0.0, 1u, 0u);
			sample = weights.x * GiUnpackRgba16(gGiRadiance[GiRadianceIndex(levelFirst, clipLevel, uint3(voxel), levelSize, faces.x)]) +
					 weights.y * GiUnpackRgba16(gGiRadiance[GiRadianceIndex(levelFirst, clipLevel, uint3(voxel), levelSize, faces.y)]) +
					 weights.z * GiUnpackRgba16(gGiRadiance[GiRadianceIndex(levelFirst, clipLevel, uint3(voxel), levelSize, faces.z)]);
		}
		// a half voxel step takes half a voxel's share of its opacity (1 - (1 - a)^(1/2)) and of its radiance (L a: the
		// radiance is premultiplied), so a partly occupied coarse voxel is seen by its occupancy over a whole voxel, not
		// its square (two steps of 1 - a: an oblique wall at 30% of a coarse voxel blocked 51%, and where a probe's rays
		// crossed such voxels differed from probe to probe)
		{
			float a = saturate(sample.a);
			float af = 1.0 - sqrt(1.0 - a);
			radiance += transmittance * (a > 0.0 ? sample.rgb / a * af : 0.0);
			transmittance *= 1.0 - af;
		}
		t += step;
	}
	return float4(radiance, transmittance);
}

float4 GiMarch(GiData gi, uint level, float3 origin, float3 direction, float start, float end)
{
	return GiMarch(gi, level, false, origin, direction, start, end);
}

// how far along a ray from origin, at most distance, to the first voxel something is in (level 0's occupancy), or
// distance where there is none. each voxel the ray crosses visited once (Amanatides and Woo), so no thin wall is stepped
// over; where it leaves the clip it goes on through the coarser ones. backface says whether the voxel met was a surface's
// seen from behind (its normal along the ray: the ray came from inside what the surface bounds; not a thin wall's voxel,
// which both faces wrote, nor the voxel the ray starts in), which tells a probe it is inside something (see
// ComputeGiProbeUpdate). with box, a ray leaves a clip at its box around the eye (as the radiance march does, so that what
// a ray sees of the coarser voxels changes with the eye: the shadow rays); without, at the clip's edge (the probes'
// depth: a probe below the finest clip's box, over its floor, traced its depth in the coarser clip from the first step,
// whose shell around the floor it stood on, so its depth read 0 that way and it was dead). the ray starts a thousandth
// of a voxel in, so that from a voxel corner (the probes' lattice points) the voxel it starts in is the one it enters,
// not whichever floor picks; and in a coarser clip the voxel it starts in doesn't count: a point outside a surface's
// shell in the finer clip is inside the thicker shell of the coarser
float GiOccluderHit(GiData gi, float3 origin, float3 direction, float distance, bool box, out bool backface)
{
	backface = false;
	float travelled = 0.0;
	for (uint clipIt = gi.clip; clipIt < gi.clipCount; clipIt++)
	{
		GiClipView clip = GiClipViewAt(clipIt);
		uint3 size = clip.volumeSize.xyz;
		float3 start = (origin + direction * travelled - clip.volumeOrigin.xyz) / clip.volumeOrigin.w + direction * 1e-3;
		int3 voxel = int3(floor(start));
		int3 stepDirection = int3(select(direction >= 0.0, 1, -1));
		float3 inverse = 1.0 / max(abs(direction), 1e-8);
		// the distance (in voxels) along the ray to the next boundary per axis, and between boundaries
		float3 next = (select(direction >= 0.0, float3(voxel + 1) - start, start - float3(voxel))) * inverse;
		float3 delta = inverse;
		float end = (distance - travelled) / clip.volumeOrigin.w;
		// and where it leaves the clip's box around the eye (see GiMarch), in its voxels
		float boxExit = box ? GiFadeBoxExit(clip.fadeCenter, clip.fadeExtent, origin + direction * travelled, direction) / clip.volumeOrigin.w : 1e30;
		float t = 0.0;
		bool left = false;
		bool first = clipIt > gi.clip;
		while (t < end)
		{
			if (!GiInside(voxel, size) || t > boxExit)
			{
				left = true;
				break;
			}
			GiVoxel found = gGiVoxels[clip.voxelFirst + GiVoxelIndex(uint3(voxel), size)];
			if ((found.albedo >> 24) != 0u && !first)
			{
				float3 normal = GiFromOctahedral(float2(found.normal & 0xFFFFu, found.normal >> 16) / 65535.0 * 2.0 - 1.0);
				backface = travelled + t > 0.0 && (found.emissionB >> 16) != 3u && dot(normal, direction) > 0.0;
				return travelled + t * clip.volumeOrigin.w;
			}
			first = false;
			if (next.x < next.y && next.x < next.z)
			{
				t = next.x;
				next.x += delta.x;
				voxel.x += stepDirection.x;
			}
			else if (next.y < next.z)
			{
				t = next.y;
				next.y += delta.y;
				voxel.y += stepDirection.y;
			}
			else
			{
				t = next.z;
				next.z += delta.z;
				voxel.z += stepDirection.z;
			}
		}
		if (!left)
			return distance;
		travelled += t * clip.volumeOrigin.w;
	}
	return distance;
}

float GiOccluderDistance(GiData gi, float3 origin, float3 direction, float distance)
{
	bool backface;
	return GiOccluderHit(gi, origin, direction, distance, true, backface);
}

// the walk again, telling where it ended (for the probe dump): the distance, the clip, the steps taken in it, and the
// voxel it ended in (its y)
float4 GiOccluderHitDebug(GiData gi, float3 origin, float3 direction, float distance)
{
	float travelled = 0.0;
	for (uint clipIt = gi.clip; clipIt < gi.clipCount; clipIt++)
	{
		GiClipView clip = GiClipViewAt(clipIt);
		uint3 size = clip.volumeSize.xyz;
		float3 start = (origin + direction * travelled - clip.volumeOrigin.xyz) / clip.volumeOrigin.w;
		int3 voxel = int3(floor(start));
		int3 stepDirection = int3(select(direction >= 0.0, 1, -1));
		float3 inverse = 1.0 / max(abs(direction), 1e-8);
		float3 next = (select(direction >= 0.0, float3(voxel + 1) - start, start - float3(voxel))) * inverse;
		float3 delta = inverse;
		float end = (distance - travelled) / clip.volumeOrigin.w;
		float boxExit = GiFadeBoxExit(clip.fadeCenter, clip.fadeExtent, origin + direction * travelled, direction) / clip.volumeOrigin.w;
		float t = 0.0;
		bool left = false;
		uint steps = 0;
		while (t < end)
		{
			if (!GiInside(voxel, size) || t > boxExit)
			{
				left = true;
				break;
			}
			GiVoxel found = gGiVoxels[clip.voxelFirst + GiVoxelIndex(uint3(voxel), size)];
			if ((found.albedo >> 24) != 0u)
				return float4(travelled + t * clip.volumeOrigin.w, float(clipIt), float(steps), float(voxel.y));
			steps++;
			if (next.x < next.y && next.x < next.z)
			{
				t = next.x;
				next.x += delta.x;
				voxel.x += stepDirection.x;
			}
			else if (next.y < next.z)
			{
				t = next.y;
				next.y += delta.y;
				voxel.y += stepDirection.y;
			}
			else
			{
				t = next.z;
				next.z += delta.z;
				voxel.z += stepDirection.z;
			}
		}
		if (!left)
			return float4(distance, float(clipIt), float(steps), -1.0);
		travelled += t * clip.volumeOrigin.w;
	}
	return float4(distance, 99.0, 0.0, -1.0);
}

// whether the voxels let a ray through from origin, a distance along direction (a shadow ray)
float GiVisibility(GiData gi, float3 origin, float3 direction, float distance)
{
	return GiOccluderDistance(gi, origin, direction, distance) < distance ? 0.0 : 1.0;
}

// whether a voxel of a clip being voxelized scrolled in (wasn't in it the frame before: see GiData::scroll)
bool GiScrolledIn(GiData gi, int3 voxel)
{
	return !GiInside(voxel + gi.scroll.xyz, GiLevelSize(gi, 0));
}

// a block's first word in gGiFlags (see SHADER_TYPES_GI_FLAG_WORD_COUNT)
uint GiFlagIndex(GiData gi, uint block)
{
	return gi.flagFirst + block * SHADER_TYPES_GI_FLAG_WORD_COUNT;
}

// voxelizing: each voxel the static one kept at its place before the clip moved, or empty where it scrolled in (all of
// the clip when it is voxelized again)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiShift(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	int3 at = int3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	int3 from = at + gi.scroll.xyz;
	gGiVoxels[gi.voxelFirst + threadId.x] =
		GiInside(from, size) ? gGiStaticVoxels[gi.voxelFirst + GiVoxelIndex(uint3(from), size)] : (GiVoxel)0;
}

uint GiPackRgba8(float4 v)
{
	uint4 c = uint4(round(saturate(v) * 255.0));
	return c.x | (c.y << 8) | (c.z << 16) | (c.w << 24);
}

float4 GiUnpackRgba8(uint v)
{
	return float4(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, v >> 24) / 255.0;
}

// voxelizing: a thread per triangle of what doesn't move (gGiJobs, by their first triangles; giStep jobs), at each of
// its instances: its surface sampled at under half a voxel apart, each voxel a sample lands in taking the triangle's
// albedo, normal and emission (the largest of each, so that which triangle wins doesn't depend on the order)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiVoxelize(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	uint frameIndex = gPushConstants.frameIndex;
	GiData gi = GiClip(gPushConstants.giClip);
	// the jobs from cullJobBase (what doesn't move, or what does)
	uint jobBase = gPushConstants.cullJobBase;
	uint jobCount = gPushConstants.giStep;
	// past the last job's triangles (the jobs end with the triangle count)
	if (threadId.x >= gGiJobs[frameIndex][jobBase + jobCount].x)
		return;
	// the job: the last whose first triangle is at most this one
	uint low = 0;
	uint high = jobCount;
	while (high - low > 1)
	{
		uint middle = (low + high) / 2;
		if (gGiJobs[frameIndex][jobBase + middle].x <= threadId.x)
			low = middle;
		else
			high = middle;
	}
	uint4 job = gGiJobs[frameIndex][jobBase + low];

	uint triangle = threadId.x - job.x;
	DrawRecord record = gDrawRecords[frameIndex][job.z];
	uint indices[3];
	for (uint cornerIt = 0; cornerIt < 3; cornerIt++)
		indices[cornerIt] = gIndexBuffer[job.y + triangle * 3 + cornerIt];
	// the triangle's albedo and emission (see below), once an instance of it has voxels to write
	bool shaded = false;
	float3 albedo = 0.0;
	float3 emission = 0.0;

	uint3 size = GiLevelSize(gi, 0);
	uint instanceCount = job.w & 0x7FFFFFFFu;
	bool moving = (job.w >> 31) != 0u; // a job of what moves (see Renderer::InternalPlanGi)
	for (uint instanceIt = 0; instanceIt < instanceCount; instanceIt++)
	{
		// as the vertex stage places them (what moves: morphed and skinned); what doesn't, by the instance's transform
		// alone (the positions and one transform, not each vertex's all: a moving clip's voxelizing reads every triangle
		// of the draws reaching what scrolled in)
		float3 p0;
		float3 p1;
		float3 p2;
		if (moving)
		{
			p0 = WorldPosition(record, indices[0], instanceIt, frameIndex);
			p1 = WorldPosition(record, indices[1], instanceIt, frameIndex);
			p2 = WorldPosition(record, indices[2], instanceIt, frameIndex);
		}
		else
		{
			float4x4 transform = gModelInstances[frameIndex][record.modelInstanceId + instanceIt].modelTransform;
			p0 = mul(transform, float4(gVertexBuffer[indices[0]].position, 1.0)).xyz;
			p1 = mul(transform, float4(gVertexBuffer[indices[1]].position, 1.0)).xyz;
			p2 = mul(transform, float4(gVertexBuffer[indices[2]].position, 1.0)).xyz;
		}
		float3 normal = cross(p1 - p0, p2 - p0);
		if (dot(normal, normal) <= 0.0)
			continue;
		normal = normalize(normal);
		// a static triangle all of whose voxels in the clip (a voxel around it, for the offset) were kept skips the
		// sampling: only what scrolled in is voxelized. its bounds clipped to the clip first: the finest clip is a few
		// meters tall, and the walls leaving it at its top or bottom were sampled whole (Bistro: 3.3 ms a scroll)
		if (!moving)
		{
			float3 low = max(GiVoxelCoordinate(gi, min(p0, min(p1, p2)), 0) - 1.0, 0.0);
			float3 high = min(GiVoxelCoordinate(gi, max(p0, max(p1, p2)), 0) + 1.0, float3(size));
			if (any(high < low))
				continue;
			float3 keptLow = max(float3(-gi.scroll.xyz), 0.0);
			float3 keptHigh = min(float3(int3(size) - gi.scroll.xyz), float3(size));
			if (all(low >= keptLow) && all(high <= keptHigh))
				continue;
		}
		// the material's, with its textures at the centroid, at a level of about a sample's size
		if (!shaded)
		{
			shaded = true;
			MaterialData material = gMaterialData[record.materialId];
			VertexP3fN3fTa4fT014fC4f vertices[3] = {gVertexBuffer[indices[0]], gVertexBuffer[indices[1]], gVertexBuffer[indices[2]]};
			float4 color = (vertices[0].color + vertices[1].color + vertices[2].color) / 3.0 * material.color;
			float4 texCoord01 = (vertices[0].texCoord01 + vertices[1].texCoord01 + vertices[2].texCoord01) / 3.0;
			if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
			{
				TextureView view = gTextureViews[material.baseColorView];
				color *= gTextures[view.textureId].SampleLevel(gSamplers[view.samplerId], ViewTexCoord(view, texCoord01), 6.0);
			}
			// metallic is the factor times the texture's (its green, see image::Usage::kMetallicRoughness), as the shading
			// has it: by the factor alone (1 by default, the texture saying where), Sponza's voxels were all black
			float metallic = material.metallic;
			if ((material.flags & MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE) != 0u)
			{
				TextureView view = gTextureViews[material.metallicRoughnessView];
				metallic *= gTextures[view.textureId].SampleLevel(gSamplers[view.samplerId], ViewTexCoord(view, texCoord01), 6.0).g;
			}
			albedo = color.rgb * (1.0 - metallic);
			emission = material.emissive.rgb;
			if ((material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
			{
				TextureView view = gTextureViews[material.emissiveView];
				emission *= gTextures[view.textureId].SampleLevel(gSamplers[view.samplerId], ViewTexCoord(view, texCoord01), 6.0).rgb;
			}
		}
		float longest = max(length(p1 - p0), max(length(p2 - p1), length(p0 - p2)));
		uint steps = clamp(uint(ceil(longest / (gi.volumeOrigin.w * 0.5))), 1u, 512u);
		// a voxel behind each of its faces, half of one off it (one on a voxel boundary would round to either side),
		// facing away from it: which face is seen and lit isn't known here (a double sided material's either, and a
		// triangle's winding, its vertex normals and which side it is drawn from needn't agree)
		// where its samples may write (voxel coordinates, a voxel of margin): the clip, or, scrolled along one axis, that
		// axis's slab that scrolled in
		float3 boxLow = -1.0;
		float3 boxHigh = float3(size) + 1.0;
		int3 scrolled = int3(gi.scroll.xyz != 0) * int3(abs(gi.scroll.xyz) < int3(size));
		if (!moving && scrolled.x + scrolled.y + scrolled.z == 1)
		{
			uint axis = scrolled.x != 0 ? 0u : (scrolled.y != 0 ? 1u : 2u);
			int scroll = gi.scroll[axis];
			boxLow[axis] = scroll > 0 ? float(int(size[axis]) - scroll) - 1.0 : -1.0;
			boxHigh[axis] = scroll > 0 ? float(size[axis]) + 1.0 : float(-scroll) + 1.0;
		}
		for (uint sideIt = 0; sideIt < 2u; sideIt++)
		{
			float3 facing = sideIt == 0 ? normal : -normal;
			float offset = 0.5;
			// the voxel behind this face stands for the other face, the one on its own side: a ray from that side meets it
			// first, and it is lit from there (its shadow rays leave away from the surface). it took this face's normal,
			// toward the surface: each shadow ray started in its twin across the surface and was blocked (no voxel was ever
			// lit by a light), and a ray from in front of a wall met the voxel of its back face, lit from inside the wall
			float2 octahedral = GiOctahedral(-facing) * 0.5 + 0.5;
			GiVoxel voxel;
			voxel.albedo = GiPackRgba8(float4(albedo, 1.0));
			voxel.normal = (uint(round(octahedral.x * 65535.0))) | (uint(round(octahedral.y * 65535.0)) << 16);
			voxel.emissionRG = GiPackHalf2(emission.rg);
			voxel.emissionB = GiPackHalf2(float2(emission.b, 0.0));
			// in voxel coordinates, the samples q = start + edgeA a + edgeB b; each row (a) only over the b whose samples
			// are in the box: a big triangle in a fine clip was sampled far outside it, and one reaching into a slab
			// that scrolled in all over (a ground triangle at the finest clip's half voxel spacing: thousands of samples)
			float3 start = GiVoxelCoordinate(gi, p0 - facing * (offset * gi.volumeOrigin.w), 0);
			float3 edgeA = (p1 - p0) / (gi.volumeOrigin.w * float(steps));
			float3 edgeB = (p2 - p0) / (gi.volumeOrigin.w * float(steps));
			for (uint a = 0; a <= steps; a++)
			{
				float3 row = start + edgeA * float(a);
				float low = 0.0;
				float high = float(steps - a);
				for (uint axisIt = 0; axisIt < 3; axisIt++)
				{
					if (abs(edgeB[axisIt]) < 1e-6)
					{
						if (row[axisIt] < boxLow[axisIt] || row[axisIt] > boxHigh[axisIt])
							high = -1.0;
						continue;
					}
					float first = (boxLow[axisIt] - row[axisIt]) / edgeB[axisIt];
					float last = (boxHigh[axisIt] - row[axisIt]) / edgeB[axisIt];
					low = max(low, min(first, last));
					high = min(high, max(first, last));
				}
				if (high < low)
					continue;
				// a row's samples are half a voxel apart: the ones landing in the voxel before skip its atomics
				int3 previous = int3(-1);
				for (uint b = uint(max(floor(low), 0.0)); b <= uint(ceil(high)) && a + b <= steps; b++)
				{
					int3 at = int3(floor(row + edgeB * float(b)));
					if (all(at == previous))
						continue;
					previous = at;
					// what doesn't move only where it scrolled in (the rest is shifted, see ComputeGiShift); what moves
					// everywhere
					if (!GiInside(at, size) || (!moving && !GiScrolledIn(gi, at)))
						continue;
					uint index = gi.voxelFirst + GiVoxelIndex(uint3(at), size);
					// something moving in this block this frame (see gGiFlags' second word; a plain store of the same)
					if (moving)
						gGiFlags[GiFlagIndex(gi, GiVoxelIndex(uint3(at) >> SHADER_TYPES_GI_FLAG_LEVEL, GiLevelSize(gi, SHADER_TYPES_GI_FLAG_LEVEL))) + 1u] = gi.frameStamp;
					InterlockedMax(gGiVoxels[index].albedo, voxel.albedo);
					InterlockedMax(gGiVoxels[index].normal, voxel.normal);
					InterlockedMax(gGiVoxels[index].emissionRG, voxel.emissionRG);
					InterlockedMax(gGiVoxels[index].emissionB, voxel.emissionB);
					// and which of the triangle's faces wrote it (bits 16 and 17): both, the voxel is a thin wall's, whose
					// normal is either face's (see GiOccluderHit)
					InterlockedOr(gGiVoxels[index].emissionB, (sideIt + 1u) << 16);
				}
			}
		}
	}
}

// the lights' light off a voxel (at, of a clip): its emission plus its albedo over pi times the light reaching it from
// the lights of its cell, each shadowed by a ray through the voxels (the shadow atlas only covers what the camera sees)
float3 GiDirectRadiance(GiData gi, uint3 at, GiVoxel voxel, float4 albedo)
{
	uint frameIndex = gPushConstants.frameIndex;
	uint3 size = GiLevelSize(gi, 0);
	float3 position = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	float3 normal = GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0);
	float3 emission = float3(GiUnpackHalf2(voxel.emissionRG), GiUnpackHalf2(voxel.emissionB).x);

	uint3 cell = min(uint3(float3(at) / float3(size) * float(SHADER_TYPES_GI_LIGHT_GRID)), uint3(SHADER_TYPES_GI_LIGHT_GRID - 1));
	uint2 range = gGiLightCells[frameIndex][gi.lightCellFirst + (cell.z * SHADER_TYPES_GI_LIGHT_GRID + cell.y) * SHADER_TYPES_GI_LIGHT_GRID + cell.x];
	float3 irradiance = 0.0;
	for (uint lightIt = range.x; lightIt < range.x + range.y; lightIt++)
	{
		LightData light = gLights[gGiLightIndices[frameIndex][lightIt]];
		float3 toLight;
		float distance = 1.0e30;
		float attenuation = 1.0;
		if (light.type == LIGHT_TYPE_DIRECTIONAL)
			toLight = -light.direction.xyz;
		else
		{
			float3 offset = light.positionRange.xyz - position;
			float distance2 = max(dot(offset, offset), 1e-8);
			distance = sqrt(distance2);
			toLight = offset / distance;
			attenuation = 1.0 / distance2;
			if (light.positionRange.w > 0.0)
			{
				float ratio = distance / light.positionRange.w;
				float window = saturate(1.0 - ratio * ratio * ratio * ratio);
				attenuation *= window * window;
			}
			if (light.type == LIGHT_TYPE_SPOT)
			{
				float cone = saturate(dot(light.direction.xyz, -toLight) * light.spotScale + light.spotOffset);
				attenuation *= cone * cone;
			}
		}
		// from the front only: a wall's voxel is lit by what shines on it inside, not by the sun outside it
		float nDotL = dot(normal, toLight);
		if (attenuation * nDotL <= 0.0)
			continue;
		// the shadow rays start a voxel further from the surface (the voxel is on the side it stands for, its twin across it)
		float visibility = GiVisibility(gi, position + normal * gi.volumeOrigin.w, toLight, distance - gi.volumeOrigin.w);
		irradiance += light.intensity.rgb * attenuation * nDotL * visibility;
	}
	return emission + albedo.rgb / 3.14159265 * irradiance;
}

// the static voxels' direct radiance (GiData::directFirst), when the clip is voxelized or the lights change: before what
// moves is voxelized over them, so that nothing moving shadows it for good
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiLightStatic(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	GiVoxel voxel = gGiVoxels[gi.voxelFirst + threadId.x];
	float4 albedo = GiUnpackRgba8(voxel.albedo);
	float3 radiance = 0.0;
	if (albedo.a != 0.0)
		radiance = GiDirectRadiance(gi, uint3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y)), voxel, albedo);
	gGiRadiance[gi.directFirst + threadId.x] = GiPackRgba16(float4(radiance, albedo.a));
	// and level 0 from nothing: the frame's lighting skips the blocks nothing is near, whose voxels must hold 0
	gGiRadiance[GiLevelFirst(gi, 0) + threadId.x] = uint2(0, 0);
}

// before what moves is voxelized over the static voxels: the blocks it was voxelized in last frame (see gGiFlags'
// second word) take the static voxels back, a thread per block (copying the whole clip took 100 MB a frame)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiRestore(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 blocks = GiLevelSize(gi, SHADER_TYPES_GI_FLAG_LEVEL);
	if (threadId.x >= blocks.x * blocks.y * blocks.z)
		return;
	if (gGiFlags[GiFlagIndex(gi, threadId.x) + 1u] + 1u != gi.frameStamp)
		return;
	uint3 block = uint3(threadId.x % blocks.x, (threadId.x / blocks.x) % blocks.y, threadId.x / (blocks.x * blocks.y));
	uint3 size = GiLevelSize(gi, 0);
	for (uint voxelIt = 0; voxelIt < 64u; voxelIt++)
	{
		uint3 at = block * 4u + uint3(voxelIt & 3u, (voxelIt >> 2) & 3u, voxelIt >> 4);
		if (any(at >= size))
			continue;
		uint index = gi.voxelFirst + GiVoxelIndex(at, size);
		gGiVoxels[index] = gGiStaticVoxels[index];
	}
}

// lighting the voxels each frame: a static voxel's direct radiance as kept (a moving one's made now: it differs from
// the static voxel at its place), plus what its albedo reflects of the probes' light at it as the last frame resolved it
// (GiData::bounce): the light bounces on, a bounce a frame. a moving voxel doesn't shadow the static ones' kept light
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiLight(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;

	uint3 at = uint3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	// a block nothing was near last frame (its flags' bit 0; static voxels don't change between voxelizings) and nothing
	// moving was in this frame or the last holds empty voxels, whose radiance is 0 since the clip was lit (see
	// ComputeGiLightStatic): skipped, voxel unread. not after a voxelizing or a reset, when the flags are of before it
	uint flagIndex = GiFlagIndex(gi, GiVoxelIndex(at >> SHADER_TYPES_GI_FLAG_LEVEL, GiLevelSize(gi, SHADER_TYPES_GI_FLAG_LEVEL)));
	if (gi.reset == 0u && all(gi.scroll.xyz == 0))
	{
		uint movingStamp = gGiFlags[flagIndex + 1u];
		if ((gGiFlags[flagIndex] & 1u) == 0u && movingStamp != gi.frameStamp && movingStamp + 1u != gi.frameStamp)
			return;
	}
	uint index = gi.voxelFirst + threadId.x;
	GiVoxel voxel = gGiVoxels[index];
	float4 albedo = GiUnpackRgba8(voxel.albedo);
	if (albedo.a == 0.0)
	{
		gGiRadiance[GiLevelFirst(gi, 0) + threadId.x] = uint2(0, 0);
		return;
	}
	// its block holds something this frame (see gGiFlags): a plain store, every voxel of the block storing the same (an
	// atomic maximum kept whatever a new buffer held, and a block it said was stamped in some far frame never counted)
	gGiFlags[flagIndex] = gi.frameStamp << SHADER_TYPES_GI_FLAG_STAMP_SHIFT;
	GiVoxel kept = gGiStaticVoxels[index];
	float3 radiance;
	// the kept light, unless a moving block lies between the voxel and a light that lights it (its shadow)
	if (kept.albedo == voxel.albedo && kept.normal == voxel.normal && !(gi.moving != 0u && GiMovingShadows(gi, at, voxel)))
		radiance = GiUnpackRgba16(gGiRadiance[gi.directFirst + threadId.x]).rgb;
	else
		radiance = GiDirectRadiance(gi, at, voxel, albedo);
	// not on a reset: the volumes hold nothing yet
	if (gi.bounce != 0u && gi.reset == 0u)
	{
		float3 position = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
		float3 normal = GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0);
		radiance += albedo.rgb * max(GiIrradianceResolved(gi, position, normal), 0.0);
	}
	gGiRadiance[GiLevelFirst(gi, 0) + threadId.x] = GiPackRgba16(float4(radiance, 1.0));
}

// a level of the radiance from the one below (radiance times occupancy, and occupancy), anisotropic: per voxel 6 faces,
// one per axis direction, each what a ray travelling that way sees through it: per lane of 2 children along the axis,
// the nearer one over the farther (front to back), the 4 lanes' mean. the mean of the 8 children, the same from every
// side, let a thin surface's lit face through to the other side (the sun on the outside of Sponza's curtains and walls
// lit its arcades' columns from inside, red and yellow, which rays traced at level 0 didn't). a thread per voxel makes
// all 6 of its faces. the levels up to the flags' skip the blocks that hold nothing this frame (their block's stamp, see
// gGiFlags), writing nothing where they held nothing last frame either (the voxels are 0 from then; the levels above
// are few); not on a reset, when the buffer may hold anything
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiMip(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint level = gPushConstants.giStep;
	uint3 size = GiLevelSize(gi, level);
	uint voxelCount = size.x * size.y * size.z;
	if (threadId.x >= voxelCount)
		return;
	int3 at = int3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	uint levelFirst = GiLevelFirst(gi, level);
	if (level <= SHADER_TYPES_GI_FLAG_LEVEL && gi.reset == 0u)
	{
		uint3 block = uint3(at) >> (SHADER_TYPES_GI_FLAG_LEVEL - level);
		uint stamp = gGiFlags[GiFlagIndex(gi, GiVoxelIndex(block, GiLevelSize(gi, SHADER_TYPES_GI_FLAG_LEVEL)))] >> SHADER_TYPES_GI_FLAG_STAMP_SHIFT;
		if (stamp != gi.frameStamp)
		{
			if (stamp + 1u == gi.frameStamp)
				for (uint face = 0; face < 6; face++)
					gGiRadiance[GiRadianceIndex(levelFirst, level, uint3(at), size, face)] = uint2(0, 0);
			return;
		}
	}
	for (uint face = 0; face < 6; face++)
	{
		uint axis = face / 2u;
		bool negative = (face & 1u) != 0u;
		float4 sum = 0.0;
		for (uint laneIt = 0; laneIt < 4; laneIt++)
		{
			uint lane0 = laneIt & 1;
			uint lane1 = laneIt >> 1;
			int3 lane = axis == 0u ? int3(0, lane0, lane1) : (axis == 1u ? int3(lane0, 0, lane1) : int3(lane0, lane1, 0));
			int3 step = int3(axis == 0u, axis == 1u, axis == 2u);
			int3 low = at * 2 + lane;
			float4 front = GiRadianceAt(gi, level - 1, negative ? low + step : low, face);
			float4 back = GiRadianceAt(gi, level - 1, negative ? low : low + step, face);
			float opening = 1.0 - saturate(front.a);
			sum += float4(front.rgb + opening * back.rgb, front.a + opening * back.a);
		}
		gGiRadiance[GiRadianceIndex(levelFirst, level, uint3(at), size, face)] = GiPackRgba16(sum / 4.0);
	}
}

// the flags (gGiFlags, see SHADER_TYPES_GI_FLAG_LEVEL): per block of voxels, after the voxels are lit (which stamps the
// blocks holding something), whether anything is voxelized in the 3x3x3 blocks around it (giStep 0: bit 0, by the
// stamps), then that spread SHADER_TYPES_GI_FLAG_REACH blocks along x, y and z in turn (steps 1 to 3: bits 1 to 3, each
// from the one before). what the voxels' resolve and the probes' update skip is read from them (one read, rather than
// the 27 of the mips a voxel, which cost 1.4 GB a frame)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiFlags(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint step = gPushConstants.giStep;
	const uint level = SHADER_TYPES_GI_FLAG_LEVEL;
	uint3 size = GiLevelSize(gi, level);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	int3 at = int3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	uint index = GiFlagIndex(gi, threadId.x);
	bool set = false;
	if (step == 0u)
	{
		for (int z = -1; z <= 1 && !set; z++)
			for (int y = -1; y <= 1 && !set; y++)
				for (int x = -1; x <= 1 && !set; x++)
				{
					int3 voxel = at + int3(x, y, z);
					if (GiInside(voxel, size) && (gGiFlags[GiFlagIndex(gi, GiVoxelIndex(uint3(voxel), size))] >> SHADER_TYPES_GI_FLAG_STAMP_SHIFT) == gi.frameStamp)
						set = true;
				}
		gGiFlags[index] = (gGiFlags[index] & ~0xFu) | (set ? 1u : 0u);
		return;
	}
	uint axis = step - 1u;
	uint from = 1u << axis;
	int3 direction = int3(axis == 0u, axis == 1u, axis == 2u);
	for (int offset = -SHADER_TYPES_GI_FLAG_REACH; offset <= SHADER_TYPES_GI_FLAG_REACH && !set; offset++)
	{
		int3 voxel = at + direction * offset;
		if (GiInside(voxel, size) && (gGiFlags[GiFlagIndex(gi, GiVoxelIndex(uint3(voxel), size))] & from) != 0u)
			set = true;
	}
	gGiFlags[index] = (gGiFlags[index] & ~(from << 1)) | (set ? from << 1 : 0u);
}

// the flags of the block a point is in (see ComputeGiFlags), 0 outside the clip
uint GiFlagsAt(GiData gi, float3 position)
{
	const uint level = SHADER_TYPES_GI_FLAG_LEVEL;
	uint3 size = GiLevelSize(gi, level);
	int3 block = int3(floor(GiVoxelCoordinate(gi, position, level)));
	if (!GiInside(block, size))
		return 0u;
	return gGiFlags[GiFlagIndex(gi, GiVoxelIndex(uint3(block), size))];
}

// whether a block something moving lies between a point and a light (a distance along direction): the blocks the line
// crosses, visited once (Amanatides and Woo), each read for the frame something moving was voxelized in it (see
// gGiFlags); this clip's only
bool GiMovingBetween(GiData gi, float3 origin, float3 direction, float distance)
{
	const uint level = SHADER_TYPES_GI_FLAG_LEVEL;
	uint3 size = GiLevelSize(gi, level);
	float blockSize = gi.volumeOrigin.w * float(1u << level);
	float3 start = (origin - gi.volumeOrigin.xyz) / blockSize;
	int3 block = int3(floor(start));
	int3 stepDirection = int3(select(direction >= 0.0, 1, -1));
	float3 inverse = 1.0 / max(abs(direction), 1e-8);
	float3 next = (select(direction >= 0.0, float3(block + 1) - start, start - float3(block))) * inverse;
	float end = distance / blockSize;
	float t = 0.0;
	while (t < end && GiInside(block, size))
	{
		if (gGiFlags[GiFlagIndex(gi, GiVoxelIndex(uint3(block), size)) + 1u] == gi.frameStamp)
			return true;
		if (next.x < next.y && next.x < next.z)
		{
			t = next.x;
			next.x += inverse.x;
			block.x += stepDirection.x;
		}
		else if (next.y < next.z)
		{
			t = next.y;
			next.y += inverse.y;
			block.y += stepDirection.y;
		}
		else
		{
			t = next.z;
			next.z += inverse.z;
			block.z += stepDirection.z;
		}
	}
	return false;
}

// whether something moving may shadow a static voxel from any of the lights that light it (as GiDirectRadiance sees
// them): the voxel's kept light is made again then, with its shadow rays
bool GiMovingShadows(GiData gi, uint3 at, GiVoxel voxel)
{
	uint frameIndex = gPushConstants.frameIndex;
	uint3 size = GiLevelSize(gi, 0);
	float3 position = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	float3 normal = GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0);
	uint3 cell = min(uint3(float3(at) / float3(size) * float(SHADER_TYPES_GI_LIGHT_GRID)), uint3(SHADER_TYPES_GI_LIGHT_GRID - 1));
	uint2 range = gGiLightCells[frameIndex][gi.lightCellFirst + (cell.z * SHADER_TYPES_GI_LIGHT_GRID + cell.y) * SHADER_TYPES_GI_LIGHT_GRID + cell.x];
	for (uint lightIt = range.x; lightIt < range.x + range.y; lightIt++)
	{
		LightData light = gLights[gGiLightIndices[frameIndex][lightIt]];
		float3 toLight;
		float distance = 1.0e30;
		if (light.type == LIGHT_TYPE_DIRECTIONAL)
			toLight = -light.direction.xyz;
		else
		{
			float3 offset = light.positionRange.xyz - position;
			distance = max(length(offset), 1e-4);
			toLight = offset / distance;
			if (light.positionRange.w > 0.0 && distance > light.positionRange.w)
				continue;
		}
		if (dot(normal, toLight) <= 0.0)
			continue;
		if (GiMovingBetween(gi, position + normal * gi.volumeOrigin.w, toLight, distance - gi.volumeOrigin.w))
			return true;
	}
	return false;
}

// the probes: a lattice over the clip (GiData::probe*), each probe's place in it (world space; a voxel corner, the
// spacing being even), its index among the clip's and the probe of an index
float3 GiProbeLatticePosition(GiData gi, uint3 probe)
{
	return gi.probeOrigin.xyz + (float3(probe) + 0.5) * gi.probeOrigin.w;
}

// a probe's offset from its place in the lattice (DDGI's relocation, see ComputeGiProbeUpdate), kept with its harmonics
// (coefficients 2 to 4's w) at a probe index of either half (see GiData::probeFirst), or none for a probe new to the
// clip (kGiNoProbe)
static const uint kGiNoProbe = 0xFFFFFFFFu;

float3 GiProbeOffset(uint offsetIndex)
{
	if (offsetIndex == kGiNoProbe)
		return 0.0;
	uint first = offsetIndex * SHADER_TYPES_GI_SH_COUNT;
	return float3(gGiIrradiance[first + 2].w, gGiIrradiance[first + 3].w, gGiIrradiance[first + 4].w);
}

// where a probe is: its place in the lattice plus its offset
float3 GiProbePosition(GiData gi, uint3 probe, uint offsetIndex)
{
	return GiProbeLatticePosition(gi, probe) + GiProbeOffset(offsetIndex);
}

// which way is out of the voxels at a voxel corner (the 8 around it), where they are a surface's: their normals' mean
// direction, or 0
float3 GiCornerAway(GiData gi, float3 position)
{
	int3 corner = int3(round(GiVoxelCoordinate(gi, position, 0)));
	uint3 size = GiLevelSize(gi, 0);
	float3 away = 0.0;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		int3 voxel = corner - 1 + int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
		if (!GiInside(voxel, size))
			continue;
		GiVoxel found = gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(voxel), size)];
		if ((found.albedo >> 24) != 0u)
			away += GiFromOctahedral(float2(found.normal & 0xFFFFu, found.normal >> 16) / 65535.0 * 2.0 - 1.0);
	}
	float length2 = dot(away, away);
	return length2 > 1e-4 ? away / sqrt(length2) : 0.0;
}

// the probe at its place last frame (the clip's scroll since), or kGiNoProbe where it was outside the clip or the clip
// was reset: where the probe's history is, in the previous half
uint GiProbeHistory(GiData gi, uint3 probe)
{
	int3 from = int3(probe) + gi.scroll.xyz / int(gi.probeSize.w);
	if (gi.reset != 0u || !GiInside(from, gi.probeSize.xyz))
		return kGiNoProbe;
	return gi.probePreviousFirst + GiProbeIndex(uint3(from), gi.probeSize);
}

uint GiProbeIndex(uint3 probe, uint4 probeSize)
{
	return (probe.z * probeSize.y + probe.y) * probeSize.x + probe.x;
}

uint3 GiProbeAt(uint index, uint4 probeSize)
{
	return uint3(index % probeSize.x, (index / probeSize.x) % probeSize.y, index / (probeSize.x * probeSize.y));
}

// the nearest probe below a point (of the 8 interpolated for it), and its fraction of the way to the next
int3 GiProbeBase(GiData gi, float3 position, out float3 fraction)
{
	float3 coordinate = (position - gi.probeOrigin.xyz) / gi.probeOrigin.w - 0.5;
	int3 base = int3(floor(coordinate));
	fraction = coordinate - float3(base);
	return base;
}

// a ray of the frame (see GiData::rayPhase): a Fibonacci sphere of rayCount directions turned by the frame's phase (a
// quaternion from a quasirandom sequence over the phases, Roberts' R3, Shoemake's rotation: every phase's set is as
// even as the Fibonacci sphere, where every 32nd point of one 32 times as big was a coarse spiral of its own, and the
// phases' light differed by a quarter, which the blending showed as a 32 frame pulse of 5%), the same for every probe
// (each probe turning the set its own way made each probe's error its own, which showed as dots where the resolve's
// weights single a probe out; the same directions everywhere, the error is shared by the probes near each other, smooth)
float3 GiRayDirection(GiData gi, uint rayIndex)
{
	float z = 1.0 - (float(rayIndex) + 0.5) * 2.0 / float(gi.rayCount);
	float radius = sqrt(max(1.0 - z * z, 0.0));
	float phi = float(rayIndex) * 2.39996323;
	float3 d = float3(radius * cos(phi), radius * sin(phi), z);
	float3 u = frac(0.5 + float(gi.rayPhase) * float3(0.8191725, 0.6710436, 0.5497005));
	float a = u.y * 6.28318531;
	float b = u.z * 6.28318531;
	float4 q = float4(sqrt(1.0 - u.x) * sin(a), sqrt(1.0 - u.x) * cos(a), sqrt(u.x) * sin(b), sqrt(u.x) * cos(b));
	return d + 2.0 * cross(q.xyz, cross(q.xyz, d) + q.w * d);
}

// the real spherical harmonics of bands 0 to 2 along a direction (band 1 in x, y, z order)
void GiShBasis(float3 d, out float basis[SHADER_TYPES_GI_SH_COUNT])
{
	basis[0] = 0.282095;
	basis[1] = 0.488603 * d.x;
	basis[2] = 0.488603 * d.y;
	basis[3] = 0.488603 * d.z;
	basis[4] = 1.092548 * d.x * d.y;
	basis[5] = 1.092548 * d.y * d.z;
	basis[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
	basis[7] = 1.092548 * d.x * d.z;
	basis[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

// a texel of a probe's depth map (SHADER_TYPES_GI_DEPTH_SIDE a side) past its edges, wrapped as the octahedral map folds:
// mirrored back across the edge, the other axis reversed
int2 GiDepthTexelWrap(int2 texel)
{
	const int side = int(SHADER_TYPES_GI_DEPTH_SIDE);
	if (texel.x < 0 || texel.x >= side)
	{
		texel.x = texel.x < 0 ? -1 - texel.x : 2 * side - 1 - texel.x;
		texel.y = side - 1 - texel.y;
	}
	if (texel.y < 0 || texel.y >= side)
	{
		texel.y = texel.y < 0 ? -1 - texel.y : 2 * side - 1 - texel.y;
		texel.x = side - 1 - texel.x;
	}
	return texel;
}

// a probe's depth map along a direction from it, bilinear: the 4 texels and their weights
struct GiDepthTaps
{
	uint texel[4];
	float weight[4];
};

GiDepthTaps GiDepthTapsAt(float3 direction)
{
	const float side = float(SHADER_TYPES_GI_DEPTH_SIDE);
	float2 st = (GiOctahedral(direction) * 0.5 + 0.5) * side - 0.5;
	int2 base = int2(floor(st));
	float2 fraction = st - float2(base);
	GiDepthTaps taps;
	for (uint cornerIt = 0; cornerIt < 4; cornerIt++)
	{
		int2 offset = int2(cornerIt & 1, cornerIt >> 1);
		int2 texel = GiDepthTexelWrap(base + offset);
		float2 weights = select(offset != 0, fraction, 1.0 - fraction);
		taps.texel[cornerIt] = uint(texel.y) * SHADER_TYPES_GI_DEPTH_SIDE + uint(texel.x);
		taps.weight[cornerIt] = weights.x * weights.y;
	}
	return taps;
}

// the probes' rays (a thread per probe and ray): each through the radiance from the probe at the level of its distance
// (see GiMarch; at level 0 only with debug 7), as far as the clipmap reaches, the environment past it; and to the first
// voxel it meets (level 0), as far as a probe's depth map tells (SHADER_TYPES_GI_DEPTH_REACH spacings)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiTrace(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint probeCount = gi.probeSize.x * gi.probeSize.y * gi.probeSize.z;
	if (threadId.x >= probeCount * gi.rayCount)
		return;
	uint probeIndex = threadId.x / gi.rayCount;
	uint rayIndex = threadId.x % gi.rayCount;
	uint3 probe = GiProbeAt(probeIndex, gi.probeSize);
	// from where the probe was last frame (its update moves it after the rays)
	float3 origin = GiProbePosition(gi, probe, GiProbeHistory(gi, probe));
	// only the probes something is voxelized near trace (see ComputeGiFlags): the others are never looked up (the lookups
	// are at surfaces, among the probes of their cell), see ComputeGiProbeUpdate
	if ((GiFlagsAt(gi, GiProbeLatticePosition(gi, probe)) & 8u) == 0u)
		return;
	float3 direction = GiRayDirection(gi, rayIndex);
	GiData last = GiClip(gi.clipCount - 1);
	float far = length(float3(last.volumeSize.xyz)) * last.volumeOrigin.w;
	float4 traced = GiMarch(gi, 0, gi.debug != 7u, origin, direction, 0.0, far);
	float3 radiance = traced.rgb + traced.a * EnvironmentRadiance(gEnvironment[0], direction, 0.6);
	// and the depth, negative where the voxel met was a surface's back (see GiOccluderHit)
	float reach = SHADER_TYPES_GI_DEPTH_REACH * gi.probeOrigin.w;
	bool backface;
	float depth = GiOccluderHit(gi, origin, direction, reach, false, backface) / gi.probeOrigin.w;
	gGiRays[gi.rayFirst + threadId.x] = GiPackRgba16(float4(radiance, backface ? -depth : depth));
}

// the probes (a group per probe, a thread per texel of its depth map): each blends this frame's rays into what it held
// last frame (the probe at its place then, shifted by the clip's scroll; a probe new to the clip starts from the coarser
// clip's light where it is, the last clip's from its rays alone): its irradiance's spherical harmonics (the first
// texel's thread: every ray weighed by its share of the sphere) and each texel of its depth map (the rays near the
// texel's direction, weighed by a power of the cosine of their angle to it; a texel no ray is near keeps what it had),
// by GiData::hysteresis, or by the frames it has accumulated while fewer. with debug 6, the reference: the irradiance
// from 512 rays through level 0 straight to the environment (seconds a frame), nothing blended
groupshared float4 gGiSharedRays[SHADER_TYPES_GI_GROUP_SIZE];
groupshared float3 gGiSharedDirections[SHADER_TYPES_GI_GROUP_SIZE];

[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiProbeUpdate(uint3 groupId: SV_GroupID, uint texelIndex: SV_GroupIndex)
{
	GiData gi = GiClip(gPushConstants.giClip);
	const uint kTexelCount = SHADER_TYPES_GI_DEPTH_SIDE * SHADER_TYPES_GI_DEPTH_SIDE;
	const uint kTexelsPerThread = SHADER_TYPES_GI_DEPTH_TEXELS_PER_THREAD;
	static_assert(kTexelCount == SHADER_TYPES_GI_GROUP_SIZE * kTexelsPerThread, "a group is a probe's depth map");
	uint probeCount = gi.probeSize.x * gi.probeSize.y * gi.probeSize.z;
	// the groups in rows, as the threads are (see GiThread)
	uint probeIndex = groupId.y * (SHADER_TYPES_GI_ROW / SHADER_TYPES_GI_GROUP_SIZE) + groupId.x;
	if (probeIndex >= probeCount)
		return;
	uint3 probe = GiProbeAt(probeIndex, gi.probeSize);
	// a probe nothing is voxelized near holds nothing (its rays weren't traced, see ComputeGiTrace): no light, and a
	// depth of 0, which no point passes
	uint first = (gi.probeFirst + probeIndex) * SHADER_TYPES_GI_SH_COUNT;
	if ((GiFlagsAt(gi, GiProbeLatticePosition(gi, probe)) & 8u) == 0u)
	{
		for (uint texelIt = 0; texelIt < kTexelsPerThread; texelIt++)
			gGiProbeDepth[gi.depthFirst + probeIndex * kTexelCount + texelIndex + texelIt * SHADER_TYPES_GI_GROUP_SIZE] = 0u;
		if (texelIndex == 0u)
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
				gGiIrradiance[first + shIt] = 0.0;
		return;
	}

	// what it held: the probe at its place last frame, if it was in the clip (and the clip wasn't reset)
	uint history = GiProbeHistory(gi, probe);
	bool shifted = history != kGiNoProbe;
	uint fromIndex = shifted ? history - gi.probePreviousFirst : 0u;
	float age = shifted ? gGiIrradiance[(gi.probePreviousFirst + fromIndex) * SHADER_TYPES_GI_SH_COUNT].w : 0.0;
	// and how much its frames' light differs from what it holds, frame to frame (coefficient 1's w: its noise)
	float noise = shifted ? gGiIrradiance[(gi.probePreviousFirst + fromIndex) * SHADER_TYPES_GI_SH_COUNT + 1].w : 0.0;

	// one more probe traced this frame (see GI_STATISTIC_ACTIVE_PROBES)
	if (texelIndex == 0u)
		InterlockedAdd(gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_ACTIVE_PROBES], 1u);
	// the rays: the thread's texels' depth (texelIndex, and every group's worth of texels after it), and (the first
	// thread) the harmonics
	float3 texelDirection[kTexelsPerThread];
	float2 depthSum[kTexelsPerThread];
	float depthWeight[kTexelsPerThread];
	float2 wideSum[kTexelsPerThread];
	float wideWeight[kTexelsPerThread];
	for (uint texelIt = 0; texelIt < kTexelsPerThread; texelIt++)
	{
		uint texel = texelIndex + texelIt * SHADER_TYPES_GI_GROUP_SIZE;
		texelDirection[texelIt] = GiDirection(uint2(texel % SHADER_TYPES_GI_DEPTH_SIDE, texel / SHADER_TYPES_GI_DEPTH_SIDE), SHADER_TYPES_GI_DEPTH_SIDE);
		depthSum[texelIt] = 0.0;
		depthWeight[texelIt] = 0.0;
		wideSum[texelIt] = 0.0;
		wideWeight[texelIt] = 0.0;
	}
	float3 sh[SHADER_TYPES_GI_SH_COUNT];
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
		sh[shIt] = 0.0;
	float basis[SHADER_TYPES_GI_SH_COUNT];
	uint backfaceCount = 0;
	float closestBackface = 1e30;
	float3 closestBackfaceDirection = 0.0;
	uint rayFirst = gi.rayFirst + probeIndex * gi.rayCount;
	float shScale = 4.0 * 3.14159265 / float(gi.rayCount);
	// the rays and their directions through shared memory, a group's worth at a time
	for (uint rayBase = 0; rayBase < gi.rayCount; rayBase += SHADER_TYPES_GI_GROUP_SIZE)
	{
		uint rayLoad = rayBase + texelIndex;
		if (rayLoad < gi.rayCount)
		{
			gGiSharedRays[texelIndex] = GiUnpackRgba16(gGiRays[rayFirst + rayLoad]);
			gGiSharedDirections[texelIndex] = GiRayDirection(gi, rayLoad);
		}
		GroupMemoryBarrierWithGroupSync();
		uint rayEnd = min(gi.rayCount - rayBase, SHADER_TYPES_GI_GROUP_SIZE);
		for (uint rayIt = 0; rayIt < rayEnd; rayIt++)
		{
			float4 ray = gGiSharedRays[rayIt];
			float3 direction = gGiSharedDirections[rayIt];
			float rayDepth = abs(ray.a);
			for (uint texelIt = 0; texelIt < kTexelsPerThread; texelIt++)
			{
				float cosine = max(dot(texelDirection[texelIt], direction), 0.0);
				float weight = pow(cosine, SHADER_TYPES_GI_DEPTH_SHARPNESS);
				depthSum[texelIt] += weight * float2(rayDepth, rayDepth * rayDepth);
				depthWeight[texelIt] += weight;
				// and by a wide cone, for a texel no ray is near (below)
				float wide = cosine * cosine * cosine * cosine;
				wideSum[texelIt] += wide * float2(rayDepth, rayDepth * rayDepth);
				wideWeight[texelIt] += wide;
			}
			if (texelIndex == 0u)
			{
				GiShBasis(direction, basis);
				for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
					sh[shIt] += ray.rgb * basis[shIt] * shScale;
				// the rays that met a surface's back, and the nearest such
				if (ray.a < 0.0)
				{
					backfaceCount++;
					if (rayDepth < closestBackface)
					{
						closestBackface = rayDepth;
						closestBackfaceDirection = direction;
					}
				}
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
	for (uint texelIt = 0; texelIt < kTexelsPerThread; texelIt++)
	{
		uint texel = texelIndex + texelIt * SHADER_TYPES_GI_GROUP_SIZE;
		// a texel's depth from the rays near it; one no ray is near this frame keeps what it held, or, holding nothing
		// yet (a probe new to the clip, or just reset), takes the rays of a wide cone around it (cos^4: a fifth of the
		// sphere), so that its map has no holes: a hole is a direction the probe sees nothing in, which the test took as
		// seeing nothing at all, and with every probe of a cell new after a scroll, a point took their mean unweighted,
		// the sunlit probes beyond the walls too (the leaks that followed the camera); and a texel's first rays count
		// whole rather than by the probe's age (blended in by it, a texel first hit at age 10 held a tenth of its
		// distance, and the probe failed the test that way for a second: the dark patches that followed the camera)
		// the wide cone's value is provisional (its mean negated) until the first rays near the texel replace it whole:
		// blended in at the hysteresis, a texel filled wide kept a wrong distance for seconds (the wide cone of a probe
		// above a floor is the floor's distance sideways too)
		bool near = depthWeight[texelIt] > 1e-4;
		float2 depth = near ? depthSum[texelIt] / depthWeight[texelIt] : (wideWeight[texelIt] > 1e-4 ? float2(-wideSum[texelIt].x, wideSum[texelIt].y) / wideWeight[texelIt] : 0.0);
		if (shifted)
		{
			float2 held = GiUnpackHalf2(gGiProbeDepth[gi.depthPreviousFirst + fromIndex * kTexelCount + texel]);
			float alpha = held.x > 0.0 ? (near ? max(1.0 - gi.hysteresis, 1.0 / (age + 1.0)) : 0.0) : (near || held.x == 0.0 ? 1.0 : 0.0);
			depth = lerp(held, depth, alpha);
		}
		gGiProbeDepth[gi.depthFirst + probeIndex * kTexelCount + texel] = GiPackHalf2(depth);
	}
	if (texelIndex != 0u)
		return;

	// where it is: its offset from its lattice point as it was (the rays were traced from there), moved on (DDGI's
	// relocation, kept per probe): out of the voxels at its corner along their normals, as far as it may go (a probe on
	// a wall's voxel shell saw nothing and was skipped, and the points nearest it took the mean of the others; a step of
	// one and a half voxels, stopping as soon as the corner was clear, left it the surface's and drew dots on CornellBox's
	// ceiling), else, where more than a quarter of its rays met a surface's back (it is inside something: Sponza's outer
	// walls are 0.6 m thick, their insides empty between the two shells, and the probes in them lit the corridors behind
	// its curtains with nothing), away from the nearest such by its distance and a tenth of a spacing; never more than
	// 0.45 of a spacing from the lattice point (its cell's points still interpolate it)
	float3 offset = GiProbeOffset(history);
	{
		float3 corner = GiCornerAway(gi, GiProbeLatticePosition(gi, probe) + offset);
		if (any(corner != 0.0))
			offset = corner * (0.45 * gi.probeOrigin.w);
		else if (backfaceCount * 4u > gi.rayCount)
			offset -= closestBackfaceDirection * ((closestBackface + 0.1) * gi.probeOrigin.w);
		float limit = 0.45 * gi.probeOrigin.w;
		float offsetLength = length(offset);
		if (offsetLength > limit)
			offset *= limit / offsetLength;
	}
	if (gi.debug == 6u)
	{
		float3 origin = GiProbeLatticePosition(gi, probe) + offset;
		GiData last = GiClip(gi.clipCount - 1);
		float far = length(float3(last.volumeSize.xyz)) * last.volumeOrigin.w;
		const uint kRays = 512;
		EnvironmentData environment = gEnvironment[0];
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
			sh[shIt] = 0.0;
		for (uint rayIt = 0; rayIt < kRays; rayIt++)
		{
			// a Fibonacci sphere
			float z = 1.0 - (float(rayIt) + 0.5) * 2.0 / float(kRays);
			float radius = sqrt(max(1.0 - z * z, 0.0));
			float phi = float(rayIt) * 2.39996323;
			float3 direction = float3(radius * cos(phi), radius * sin(phi), z);
			float4 traced = GiMarch(gi, 0, origin, direction, 0.0, far);
			float3 radiance = traced.rgb + traced.a * EnvironmentRadiance(environment, direction, 0.6);
			GiShBasis(direction, basis);
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
				sh[shIt] += radiance * basis[shIt] * (4.0 * 3.14159265 / float(kRays));
		}
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
			gGiIrradiance[first + shIt] = float4(sh[shIt], shIt == 2u ? offset.x : (shIt == 3u ? offset.y : (shIt == 4u ? offset.z : 0.0)));
		return;
	}

	// what it held, and its irradiance blended into it
	float3 held[SHADER_TYPES_GI_SH_COUNT];
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
		held[shIt] = 0.0;
	if (shifted)
	{
		uint heldFirst = (gi.probePreviousFirst + fromIndex) * SHADER_TYPES_GI_SH_COUNT;
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
			held[shIt] = gGiIrradiance[heldFirst + shIt].rgb;
	}
	else if (gi.reset == 0u && gi.clip + 1 < gi.clipCount)
	{
		// new to the clip: the coarser clip's probes around it as they were last frame (at their places then: shifted by
		// its scroll too; none if it was voxelized whole), trilinear, as one frame's worth
		GiData above = GiClip(gi.clip + 1);
		if (all(abs(above.scroll.xyz) < int3(above.volumeSize.xyz)))
		{
			float3 fraction;
			int3 base = GiProbeBase(above, GiProbeLatticePosition(gi, probe), fraction) + above.scroll.xyz / int(above.probeSize.w);
			for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
			{
				int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
				uint3 aboveProbe = uint3(clamp(base + offset, int3(0), int3(above.probeSize.xyz) - 1));
				float3 weights = select(offset != 0, fraction, 1.0 - fraction);
				float weight = weights.x * weights.y * weights.z;
				uint aboveFirst = (above.probePreviousFirst + GiProbeIndex(aboveProbe, above.probeSize)) * SHADER_TYPES_GI_SH_COUNT;
				for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
					held[shIt] += weight * gGiIrradiance[aboveFirst + shIt].rgb;
			}
			age = 1.0;
		}
	}
	float alpha = max(1.0 - gi.hysteresis, 1.0 / (age + 1.0));
	// DDGI's adaptive hysteresis, by the probe's own noise: the frame's mean radiance (coefficient 0, luminance) against
	// what it holds, a difference far beyond what its frames usually differ by (four times, and half of what it holds)
	// is a change of the light (one switched or moved, the sun turned), blended in a quarter at a time rather than a
	// thirtieth; the usual difference is averaged over its frames (it grows through a change, so that ends the fast
	// blending on its own). only once it has settled (16 frames): until then the age blends fast anyway
	const float3 kLuminance = float3(0.2126, 0.7152, 0.0722);
	float heldLuminance = dot(held[0], kLuminance);
	float difference = abs(dot(sh[0], kLuminance) - heldLuminance);
	if (gi.adapt != 0u && shifted && age >= 16.0 && difference > max(4.0 * noise, 0.5 * heldLuminance))
		alpha = max(alpha, 0.25);
	noise = shifted ? lerp(noise, difference, 0.05) : difference;
	// and whether it is inside something still (more than a quarter of its rays met a surface's back: coefficient 5's w,
	// DDGI's classification): the resolve skips it (Sponza's outer walls are thicker than a probe may move, and a probe
	// between their shells, which sees 0.3 of a spacing all around, passed as seeing the points of the corridor beside)
	float inside = backfaceCount * 4u > gi.rayCount ? 1.0 : 0.0;
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
	{
		float kept = shIt == 0u ? min(age + 1.0, 65535.0) : (shIt == 1u ? noise : (shIt == 2u ? offset.x : (shIt == 3u ? offset.y : (shIt == 4u ? offset.z : (shIt == 5u ? inside : 0.0)))));
		gGiIrradiance[first + shIt] = float4(lerp(held[shIt], sh[shIt], alpha), kept);
	}
	// the probe dumped for debugging (see GI_DUMP_FLOAT_COUNT)
	if (probeIndex == gi.dumpProbe)
	{
		uint dumpFirst = GI_STATISTIC_COUNT + gi.clip * GI_DUMP_FLOAT_COUNT;
		float3 position = GiProbeLatticePosition(gi, probe) + offset;
		// and the lattice point in voxels, and the occupancy of the column of 8 voxels through it (from 4 below, bit 0)
		float3 latticeVoxels = GiVoxelCoordinate(gi, GiProbeLatticePosition(gi, probe), 0);
		uint column = 0;
		for (int rowIt = -4; rowIt < 4; rowIt++)
		{
			int3 voxel = int3(floor(latticeVoxels)) + int3(0, rowIt, 0);
			if (GiInside(voxel, GiLevelSize(gi, 0)) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(voxel), GiLevelSize(gi, 0))].albedo >> 24) != 0u)
				column |= 1u << uint(rowIt + 4);
		}
		// the box exit straight down and a walk straight down from where the rays were traced (both in spacings), to
		// compare with the rays' depths
		float3 traced = GiProbePosition(gi, probe, history);
		bool back;
		float values[16] = {position.x, position.y, position.z, offset.x, offset.y, offset.z, age + 1.0, inside, float(backfaceCount), float(gi.rayCount), latticeVoxels.x, latticeVoxels.y, latticeVoxels.z, float(column),
			GiFadeBoxExit(gi.fadeCenter, gi.fadeExtent, traced, float3(0, -1, 0)) / gi.probeOrigin.w,
			GiOccluderHit(gi, traced, float3(0, -1, 0), SHADER_TYPES_GI_DEPTH_REACH * gi.probeOrigin.w, false, back) / gi.probeOrigin.w};
		for (uint valueIt = 0; valueIt < 16; valueIt++)
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + valueIt] = asuint(values[valueIt]);
		float4 walk = GiOccluderHitDebug(gi, traced, float3(0, -1, 0), SHADER_TYPES_GI_DEPTH_REACH * gi.probeOrigin.w);
		for (uint valueIt = 0; valueIt < 4; valueIt++)
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + 32 + valueIt] = asuint(walk[valueIt]);
		// and after the depth map, the first 16 rays' directions' y and depths
		for (uint rayIt = 0; rayIt < 16u && rayIt < gi.rayCount; rayIt++)
		{
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + rayIt * 2] = asuint(GiRayDirection(gi, rayIt).y);
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + rayIt * 2 + 1] = asuint(GiUnpackRgba16(gGiRays[rayFirst + rayIt]).a);
		}
		for (uint texelIt = 0; texelIt < kTexelCount; texelIt++)
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + texelIt] = asuint(GiUnpackHalf2(gGiProbeDepth[gi.depthFirst + probeIndex * kTexelCount + texelIt]).x);
	}
}

// the shading's debug view (GiData::debug's numbering, from PushConstants::debugView), or 0
uint GiDebug()
{
	return DebugViewGi(DebugView());
}

// the shading's (with the debug views)
float3 GiIrradiance(float3 position, float3 normal)
{
	return GiIrradiance(position, normal, GiDebug());
}

// the debug views of the voxels (3 and 4: the radiance of the nearest voxel behind the point, or in front, along its
// normal; 5: its albedo; magenta where there is none, cyan outside the clip)
float3 GiVoxelDebug(GiData gi, float3 position, float3 normal, uint debug)
{
	float side = debug == 4u ? 1.0 : -1.0;
	for (float t = 0.25; t <= 1.75; t += 0.5)
	{
		int3 at = int3(floor(GiVoxelCoordinate(gi, position + normal * (side * t * gi.volumeOrigin.w), 0)));
		if (!GiInside(at, GiLevelSize(gi, 0)))
			return float3(0.0, 1.0, 1.0);
		GiVoxel found = GiVoxelAt(gi, uint3(at));
		if ((found.albedo >> 24) == 0u)
			continue;
		if (debug == 5u)
			return GiUnpackRgba8(found.albedo).rgb;
		return GiRadianceAt(gi, 0, at).rgb;
	}
	return float3(1.0, 0.0, 1.0);
}

// the finest clip that holds the point well inside (more than a probe spacing from its edges, where its probes are
// clamped), blended into the next over the 8 voxels after that, so the clips meet without a seam
// the surface reference (DEBUG_VIEW_GI_SURFACE_REFERENCE): the irradiance at the point itself over pi, 256 cosine weighted
// rays from it (off it two voxels along its normal, past the voxels of its back face in front of it) through the finest
// clip's voxels that holds it and the coarser ones', the environment past them
float3 GiPixelReference(float3 position, float3 normal)
{
	uint clipCount = gGi[gPushConstants.frameIndex][0].clipCount;
	uint start = clipCount - 1;
	for (uint clipIt = 0; clipIt < clipCount; clipIt++)
		if (GiClipWeight(GiClipViewAt(clipIt), position) > 0.0)
		{
			start = clipIt;
			break;
		}
	GiData gi = GiClip(start);
	GiData last = GiClip(clipCount - 1);
	float far = length(float3(last.volumeSize.xyz)) * last.volumeOrigin.w;
	float3 origin = position + normal * (2.0 * gi.volumeOrigin.w);
	// at a concave edge that is inside the other surface's voxel (its shell is a voxel thick on this side too): out of
	// it along that voxel's normal, else every ray starts in its radiance (CornellBox's red wall drew a line along the
	// floor)
	{
		int3 at = int3(floor(GiVoxelCoordinate(gi, origin, 0)));
		if (GiInside(at, GiLevelSize(gi, 0)))
		{
			GiVoxel voxel = GiVoxelAt(gi, uint3(at));
			if ((voxel.albedo >> 24) != 0u)
				origin += GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0) * (1.5 * gi.volumeOrigin.w);
		}
	}
	float3 tangent = normalize(abs(normal.y) < 0.9 ? cross(normal, float3(0, 1, 0)) : cross(normal, float3(1, 0, 0)));
	float3 bitangent = cross(normal, tangent);
	EnvironmentData environment = gEnvironment[0];
	const uint kRays = 256;
	// the spiral turned and shifted by a hash of the point: the same directions everywhere made radial streaks (each
	// ray's hit or miss the same over a whole surface); turned per point, they are noise instead
	uint hash = asuint(position.x * 7919.0) * 73856093u ^ asuint(position.y * 7919.0) * 19349663u ^ asuint(position.z * 7919.0) * 83492791u;
	hash = (hash ^ (hash >> 16)) * 0x45d9f3bu;
	float turn = float(hash & 0xFFFFu) / 65536.0 * 6.28318531;
	float shift = float(hash >> 16) / 65536.0;
	float3 sum = 0.0;
	for (uint rayIt = 0; rayIt < kRays; rayIt++)
	{
		// cosine weighted, a Fibonacci spiral over the disk
		float u = frac((float(rayIt) + shift) / float(kRays));
		float r = sqrt(u);
		float phi = float(rayIt) * 2.39996323 + turn;
		float3 direction = normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + normal * sqrt(max(1.0 - u, 0.0)));
		float4 traced = GiMarch(gi, 0, origin, direction, 0.0, far);
		sum += traced.rgb + traced.a * EnvironmentRadiance(environment, direction, 0.6);
	}
	return sum / float(kRays);
}

float3 GiIrradiance(float3 position, float3 normal, uint debug, float3 view = 0.0)
{
	if (debug == 14u)
		return GiPixelReference(position, normal);
	if (debug == 8u || debug == 9u)
		return GiClipDebug(position, normal, debug == 9u);
	if (debug == 13u)
		return GiProbesDebug(position, normal);
	if (debug == 15u)
		return GiProbeStateDebug(position, normal);
	if (debug < 2u || debug > 5u)
	{
		// as the shading looks it up, given the eye's direction
		GiSample sample = any(view != 0.0) ? GiSampleSurface(position, normal, view) : GiSampleAt(position, normal);
		return sample.valid ? GiEvaluate(sample, normal) : -1.0;
	}
	GiData gi = GiClip(0);
	if (gi.enabled == 0)
		return -1.0;
	// each clip by its weight of what the finer ones leave (as GiSampleAt); the voxel views (3 to 5) the finest that
	// lights the point at all
	float3 result = 0.0;
	float remaining = 1.0;
	float covered = 0.0;
	for (uint clipIt = 0; clipIt < gi.clipCount && remaining > 0.0; clipIt++)
	{
		float weight = GiClipWeight(GiClipViewAt(clipIt), position);
		if (weight <= 0.0)
			continue;
		float3 value = GiIrradianceAt(clipIt == 0 ? gi : GiClip(clipIt), position, normal, debug);
		if (debug >= 3u && debug <= 5u)
			return value;
		float share = remaining * weight;
		remaining -= share;
		if (any(value < 0.0))
			continue;
		result += share * value;
		covered += share;
	}
	return covered > 0.0 ? result / covered : -1.0;
}

// the irradiance at a point of a clip, from its voxels' resolved probes (or the debug views of the voxels, or with 2 the
// share of the probes around the point that it sees: see ComputeGiResolve)
float3 GiIrradianceAt(GiData gi, float3 position, float3 normal, uint debug)
{
	if (debug >= 3u && debug <= 5u)
		return GiVoxelDebug(gi, position, normal, debug);
	return GiIrradianceResolved(gi, position, normal, debug == 2u);
}

// the radiance a white Lambertian surface at position, of normal, reflects (as EnvironmentIrradiance: the irradiance
// over pi), from the 8 voxels around the point offset 1.5 voxels along its normal, trilinear (filtered): each the
// irradiance of the probes it sees, weighed by how much it sees of them (see ComputeGiResolve). a negative value where
// they see none; with weightOnly, the weight
float3 GiIrradianceResolved(GiData gi, float3 position, float3 normal, bool weightOnly = false)
{
	GiSample sample = GiSampleClip(GiClipViewAt(gi.clip), position, normal, normal * 1.5);
	if (weightOnly)
		return sample.last.w;
	return sample.valid ? GiEvaluate(sample, normal) : -1.0;
}

// per voxel of a clip, after its probes: the spherical harmonics of the 8 probes around its center, each weighed by how
// much of it the voxel sees (trilinear by its center; whether the probe sees the voxel's point, by the probe's depth map:
// Chebyshev's inequality on the mean and variance of the distance it sees that way, as DDGI tests it; and how much the
// probe faces the voxel's open side), and the sum of the weights. a point interpolating the voxels around it so sees each
// probe as much as the voxels around it do, at 4 filtered reads. a group per block of 4x4x4 voxels (a flag's block, see
// ComputeGiFlags: nothing near it, the block is skipped), whose 8 probes (the block spans one cell, the spacing being a
// multiple of 8) are read into shared memory once rather than 8 a voxel (1.2 KB each)
// a block's probes are 8: the spacing is a multiple of 8 voxels (Renderer checks), so a block never spans two cells
groupshared float4 gGiSharedSh[8 * SHADER_TYPES_GI_SH_COUNT];
groupshared uint gGiSharedDepth[8 * SHADER_TYPES_GI_DEPTH_SIDE * SHADER_TYPES_GI_DEPTH_SIDE];
groupshared float3 gGiSharedProbePosition[8];

[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiResolve(uint3 groupId: SV_GroupID, uint groupIndex: SV_GroupIndex)
{
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 size = GiLevelSize(gi, 0);
	uint3 blocks = GiLevelSize(gi, SHADER_TYPES_GI_FLAG_LEVEL);
	static_assert(SHADER_TYPES_GI_GROUP_SIZE == 64u && SHADER_TYPES_GI_FLAG_LEVEL == 2u, "a group is a block of 4x4x4 voxels");
	// the groups in rows, as the threads are (see GiThread)
	uint blockIndex = groupId.y * (SHADER_TYPES_GI_ROW / SHADER_TYPES_GI_GROUP_SIZE) + groupId.x;
	if (blockIndex >= blocks.x * blocks.y * blocks.z)
		return;
	uint3 block = uint3(blockIndex % blocks.x, (blockIndex / blocks.x) % blocks.y, blockIndex / (blocks.x * blocks.y));
	uint3 at = block * 4u + uint3(groupIndex & 3u, (groupIndex >> 2) & 3u, groupIndex >> 4);
	bool inside = all(at < size);
	uint3 texel = uint3(at.x, at.y, at.z + gi.clip * size.z);
	// only the voxels within a few of a surface are looked up (see GiSampleSurface): the rest see no probe
	if ((gGiFlags[GiFlagIndex(gi, blockIndex)] & 1u) == 0u)
	{
		if (inside)
			for (uint volumeIt = 0; volumeIt < SHADER_TYPES_GI_VOLUME_COUNT; volumeIt++)
				gGiVolumeImages[volumeIt][texel] = 0.0;
		return;
	}

	// the block's probes: from the cell of its first voxel to the last's, 2 or 3 an axis (3 only where it spans two cells:
	// never at a spacing of 8 or more, since the cells' boundaries are on the blocks'), clamped to the lattice as the
	// voxels' are
	const uint kTexelCount = SHADER_TYPES_GI_DEPTH_SIDE * SHADER_TYPES_GI_DEPTH_SIDE;
	float3 fraction;
	int3 blockBase = GiProbeBase(gi, gi.volumeOrigin.xyz + (float3(block * 4u) + 0.5) * gi.volumeOrigin.w, fraction);
	int3 blockLast = GiProbeBase(gi, gi.volumeOrigin.xyz + (float3(block * 4u) + 3.5) * gi.volumeOrigin.w, fraction);
	uint3 slots = uint3(blockLast - blockBase) + 2u;
	uint slotCount = slots.x * slots.y * slots.z;
	if (slotCount > 8u)
		return;
	for (uint loadIt = groupIndex; loadIt < slotCount * SHADER_TYPES_GI_SH_COUNT; loadIt += SHADER_TYPES_GI_GROUP_SIZE)
	{
		uint slot = loadIt / SHADER_TYPES_GI_SH_COUNT;
		uint3 probe = uint3(clamp(blockBase + int3(slot % slots.x, (slot / slots.x) % slots.y, slot / (slots.x * slots.y)), int3(0), int3(gi.probeSize.xyz) - 1));
		gGiSharedSh[loadIt] = gGiIrradiance[(gi.probeFirst + GiProbeIndex(probe, gi.probeSize)) * SHADER_TYPES_GI_SH_COUNT + loadIt % SHADER_TYPES_GI_SH_COUNT];
	}
	for (uint loadIt = groupIndex; loadIt < slotCount * kTexelCount; loadIt += SHADER_TYPES_GI_GROUP_SIZE)
	{
		uint slot = loadIt / kTexelCount;
		uint3 probe = uint3(clamp(blockBase + int3(slot % slots.x, (slot / slots.x) % slots.y, slot / (slots.x * slots.y)), int3(0), int3(gi.probeSize.xyz) - 1));
		gGiSharedDepth[loadIt] = gGiProbeDepth[gi.depthFirst + GiProbeIndex(probe, gi.probeSize) * kTexelCount + loadIt % kTexelCount];
	}
	if (groupIndex < slotCount)
	{
		uint slot = groupIndex;
		uint3 probe = uint3(clamp(blockBase + int3(slot % slots.x, (slot / slots.x) % slots.y, slot / (slots.x * slots.y)), int3(0), int3(gi.probeSize.xyz) - 1));
		gGiSharedProbePosition[slot] = GiProbePosition(gi, probe, gi.probeFirst + GiProbeIndex(probe, gi.probeSize));
	}
	GroupMemoryBarrierWithGroupSync();
	if (!inside)
		return;

	float3 center = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	GiVoxel voxel = gGiVoxels[gi.voxelFirst + GiVoxelIndex(at, size)];
	// which way is away from the surfaces next to it: the occupancy's gradient over its 6 neighbors, plus a surface's
	// voxel's own normal; the point the probes are tested from is out that way, by a fifth of a spacing (DDGI's normal
	// bias: a point close to a surface fails a probe's test at a grazing angle, where the probe's depth that way is of
	// rays into the surface before it and past it), at least a voxel (from a surface voxel's center, among its surface's
	// voxels, a probe saw nothing). the gradient for a surface's voxel too: by its normal alone, a floor's voxel at a
	// wall's base was tested from a point straight up, as close to the wall as before, which the sunlit probes beyond the
	// wall saw at a grazing angle (a bright band along Sponza's floors)
	float3 away = 0.0;
	for (uint axisIt = 0; axisIt < 3; axisIt++)
	{
		int3 step = int3(axisIt == 0u, axisIt == 1u, axisIt == 2u);
		float below = GiInside(int3(at) - step, size) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(int3(at) - step), size)].albedo >> 24) != 0u ? 1.0 : 0.0;
		float above = GiInside(int3(at) + step, size) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(int3(at) + step), size)].albedo >> 24) != 0u ? 1.0 : 0.0;
		away[axisIt] = below - above;
	}
	if ((voxel.albedo >> 24) != 0u)
	{
		float3 normal = GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0);
		away = dot(away, away) > 0.0 ? normalize(away) + normal : normal;
	}
	float awayLength = length(away);
	away = awayLength > 0.0 ? away / awayLength : away;
	// a surface's voxel is resolved as the voxel out along its open side would be (the lookups land in it where the
	// offset is small, a finer clip's voxels, and its own cell's weights admitted a probe the voxel above rejected: a
	// bright band along Sponza's floors at their walls)
	if ((voxel.albedo >> 24) != 0u)
		center += away * gi.volumeOrigin.w;
	float3 point = center + away * max(0.2 * gi.probeOrigin.w, gi.volumeOrigin.w);
	// between the block's cell's probes (the moved center may be past the cell: clamped, since only its probes are in
	// shared memory; read past them, the floor behind Sponza's fountains came out as black and colored garbage)
	int3 base = blockBase;
	fraction = saturate((center - GiProbeLatticePosition(gi, uint3(max(base, int3(0))))) / gi.probeOrigin.w);
	int3 slotBase = int3(0);
	float3 sh[SHADER_TYPES_GI_SH_COUNT];
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
		sh[shIt] = 0.0;
	float seen = 0.0;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
		uint3 probe = uint3(clamp(base + offset, int3(0), int3(gi.probeSize.xyz) - 1));
		uint3 slotAt = uint3(slotBase + offset);
		uint slot = (slotAt.z * slots.y + slotAt.y) * slots.x + slotAt.x;
		float3 weights = select(offset != 0, fraction, 1.0 - fraction);
		float weight = 1.0;
		float3 probePosition = gGiSharedProbePosition[slot];
		// the probes on the open side weigh more (wrapped, so that those beside it count a little; DDGI's fifth for the
		// ones behind let the probes below Sponza's floors, in the clipmap's margin and lit by the environment through its
		// bottom, light the floors' edges along the walls, where every probe in the corridor fails and a point takes the
		// mean of what is left)
		if (awayLength > 0.0)
		{
			float facing = dot(normalize(probePosition - center), away) * 0.5 + 0.5;
			weight *= facing * facing + 0.02;
		}
		// whether it sees the point: against the distance it sees toward the point, by Chebyshev's inequality where the
		// point is farther, cubed, at least a thousandth (DDGI's twentieth of a sunlit probe outside a wall lit the points
		// at its base inside, a band along Sponza's floors: the floor only needs to be more than nothing, since the lookup
		// normalizes, so that a point no probe sees takes their mean). a probe that sees nothing that way at all (it is
		// inside a surface's voxels: every ray of its starts in one) is skipped: the twentieth of it drew dark dots at
		// the probes
		float3 toProbe = probePosition - point;
		float distance = length(toProbe);
		GiDepthTaps taps = GiDepthTapsAt(-toProbe / max(distance, 1e-6));
		float2 moments = 0.0;
		for (uint tapIt = 0; tapIt < 4; tapIt++)
		{
			float2 tap = GiUnpackHalf2(gGiSharedDepth[slot * kTexelCount + taps.texel[tapIt]]);
			moments += taps.weight[tapIt] * float2(abs(tap.x), tap.y);
		}
		if (moments.x < 1e-3 || gGiSharedSh[slot * SHADER_TYPES_GI_SH_COUNT + 5].w > 0.5)
			continue;
		float d = distance / gi.probeOrigin.w;
		float variance = max(moments.y - moments.x * moments.x, 0.0) + 1e-4;
		float visibility = 1.0;
		if (d > moments.x)
		{
			float delta = d - moments.x;
			visibility = variance / (variance + delta * delta);
			visibility *= visibility * visibility;
		}
		weight *= max(visibility, 0.001);
		// less of the ones seen little (DDGI's crush: below a fifth, cubed), then a little of every probe, so that a point
		// none sees takes their mean rather than nothing (the floor after the crush: a floor before it was crushed to
		// 1e-11, which the volumes' half floats hold as 0, so such a point had no light at all: black patches wherever
		// every probe of a cell failed); then by where the point is between the probes (after the crush, as RTXGI has
		// it: crushing the product crushed every probe at a cell's center, where each is an eighth, and the facing ones
		// least, which drew the cells)
		if (weight < 0.2)
			weight *= weight * weight * 25.0;
		weight = max(weight, 1e-3);
		weight *= weights.x * weights.y * weights.z;
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
			sh[shIt] += weight * gGiSharedSh[slot * SHADER_TYPES_GI_SH_COUNT + shIt].rgb;
		seen += weight;
	}
	// per color its coefficients 0 to 3 and 4 to 7, then the three colors' coefficient 8 and the weight (see GiSample)
	gGiVolumeImages[0][texel] = float4(sh[0].r, sh[1].r, sh[2].r, sh[3].r);
	gGiVolumeImages[1][texel] = float4(sh[4].r, sh[5].r, sh[6].r, sh[7].r);
	gGiVolumeImages[2][texel] = float4(sh[0].g, sh[1].g, sh[2].g, sh[3].g);
	gGiVolumeImages[3][texel] = float4(sh[4].g, sh[5].g, sh[6].g, sh[7].g);
	gGiVolumeImages[4][texel] = float4(sh[0].b, sh[1].b, sh[2].b, sh[3].b);
	gGiVolumeImages[5][texel] = float4(sh[4].b, sh[5].b, sh[6].b, sh[7].b);
	gGiVolumeImages[6][texel] = float4(sh[8], seen);
}

// the irradiance's spherical harmonics (bands 0 to 2) around a point, from its clips' voxels (see GiSampleAt), as the
// volumes hold them (see ComputeGiResolve): per color its coefficients 0 to 3 and 4 to 7, and the three colors'
// coefficient 8 with the weight of the probes they saw. vectors rather than arrays: in the main pass's shader, arrays
// of the coefficients spilled (Bistro's main pass 31 ms instead of 4)
struct GiSample
{
	float4 r0;
	float4 r1;
	float4 g0;
	float4 g1;
	float4 b0;
	float4 b1;
	float4 last; // the coefficient 8s, rgb, and the weight
	bool valid;
	bool inside; // whether a clip holds the point (though none of its voxels around it saw a probe, if not valid)
};

// a clip's voxels' resolved probes around the point offset 1.5 voxels along its normal (see GiIrradianceResolved),
// normalized by their weight
// bias is in voxels of the finest clip, the same distance off the surface in every clip (in a clip's own voxels, a
// coarse clip's lookup reached into the open, past what is near the surface: brighter than the finer ones), else, where
// that finds none of its probes seen (a coarse clip's voxels around the point are the surface's), in its own voxels
GiSample GiSampleClip(GiClipView gi, float3 position, float3 normal, float3 bias)
{
	GiSample result;
	float3 size = float3(gi.volumeSize.xyz);
	SamplerState linear = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	float finest = gGi[gPushConstants.frameIndex][0].volumeOrigin.w;
	float3 uvw = 0.0;
	for (uint attemptIt = 0; attemptIt < 2; attemptIt++)
	{
		float3 biased = position + bias * (attemptIt == 0 ? finest : gi.volumeOrigin.w);
		// the clip's slab of the stacked volumes, clamped inside it (no filtering across into the next)
		float3 coordinate = (biased - gi.volumeOrigin.xyz) / gi.volumeOrigin.w;
		coordinate.z = clamp(coordinate.z, 0.5, size.z - 0.5) + float(gi.clip) * size.z;
		uvw = coordinate / float3(size.xy, size.z * float(gi.clipCount));
		result.last = gGiVolumes[6].SampleLevel(linear, uvw, 0.0);
		if (result.last.w > 1e-7 || finest == gi.volumeOrigin.w)
			break;
	}
	float weight = result.last.w;
	result.valid = weight > 1e-7;
	float scale = result.valid ? 1.0 / weight : 0.0;
	result.r0 = gGiVolumes[0].SampleLevel(linear, uvw, 0.0) * scale;
	result.r1 = gGiVolumes[1].SampleLevel(linear, uvw, 0.0) * scale;
	result.g0 = gGiVolumes[2].SampleLevel(linear, uvw, 0.0) * scale;
	result.g1 = gGiVolumes[3].SampleLevel(linear, uvw, 0.0) * scale;
	result.b0 = gGiVolumes[4].SampleLevel(linear, uvw, 0.0) * scale;
	result.b1 = gGiVolumes[5].SampleLevel(linear, uvw, 0.0) * scale;
	result.last = float4(result.last.xyz * scale, weight);
	return result;
}

// the finest clip that holds the point well inside, blended into the next at its edge (see GiIrradiance)
GiSample GiSampleAt(float3 position, float3 normal)
{
	return GiSampleAt(position, normal, normal * 1.5);
}

GiSample GiSampleAt(float3 position, float3 normal, float3 bias)
{
	GiSample none;
	none.r0 = 0.0;
	none.r1 = 0.0;
	none.g0 = 0.0;
	none.g1 = 0.0;
	none.b0 = 0.0;
	none.b1 = 0.0;
	none.last = 0.0;
	none.valid = false;
	none.inside = false;
	if (gGi[gPushConstants.frameIndex][0].enabled == 0)
		return none;
	// each clip by its weight (GiClipWeight) of what the finer ones leave, the shares of those that see probes there
	// normalized. it blended a clip with the next one's sample at whatever weight the next one had: where the finer one
	// lit a point a little (a thousandth), the next lit it all, outside its box around the eye, where what its edges
	// clamp changes as it snaps (a region switched between 61 and 91 of 255, which the clips view showed lit by the last)
	uint clipCount = gGi[gPushConstants.frameIndex][0].clipCount;
	GiSample result = none;
	float remaining = 1.0;
	float covered = 0.0;
	for (uint clipIt = 0; clipIt < clipCount && remaining > 0.0; clipIt++)
	{
		GiClipView clip = GiClipViewAt(clipIt);
		float weight = GiClipWeight(clip, position);
		if (weight <= 0.0)
			continue;
		none.inside = true;
		GiSample sample = GiSampleClip(clip, position, normal, bias);
		float share = remaining * weight;
		remaining -= share;
		if (!sample.valid)
			continue;
		result.r0 += share * sample.r0;
		result.r1 += share * sample.r1;
		result.g0 += share * sample.g0;
		result.g1 += share * sample.g1;
		result.b0 += share * sample.b0;
		result.b1 += share * sample.b1;
		result.last += share * sample.last;
		covered += share;
	}
	if (covered <= 0.0)
		return none;
	float scale = 1.0 / covered;
	result.r0 *= scale;
	result.r1 *= scale;
	result.g0 *= scale;
	result.g1 *= scale;
	result.b0 *= scale;
	result.b1 *= scale;
	result.last *= scale;
	result.valid = true;
	result.inside = true;
	return result;
}

// a surface's lookup (the shading's): off it 0.75 voxels along its normal and 1.5 toward the eye (along the line of
// sight, which is clear: at a concave edge the normal alone leads into the other surface's voxels, whose probes are the
// other surface's), and twice as far where that finds no probes
GiSample GiSampleSurface(float3 position, float3 normal, float3 view)
{
	float3 bias = normal * 0.75 + view * 1.5;
	GiSample sample = GiSampleAt(position, normal, bias);
	if (!sample.valid && sample.inside)
		sample = GiSampleAt(position, normal, bias * 2.0);
	return sample;
}

// a sample's irradiance over pi (the radiance a white Lambertian surface reflects) for a surface facing direction: along
// a reflection instead, the radiance around it in a cosine lobe (a rough reflection's)
float3 GiEvaluate(GiSample sample, float3 direction)
{
	// the basis (see GiShBasis) times the cosine lobe's convolution over pi, per band: 1, 2/3 and 1/4
	float3 d = direction;
	float4 basis0 = float4(0.282095, 2.0 / 3.0 * 0.488603 * d);
	float4 basis1 = 0.25 * float4(1.092548 * d.x * d.y, 1.092548 * d.y * d.z, 0.315392 * (3.0 * d.z * d.z - 1.0), 1.092548 * d.x * d.z);
	float basis8 = 0.25 * 0.546274 * (d.x * d.x - d.y * d.y);
	return max(
		float3(
			dot(sample.r0, basis0) + dot(sample.r1, basis1),
			dot(sample.g0, basis0) + dot(sample.g1, basis1),
			dot(sample.b0, basis0) + dot(sample.b1, basis1)) +
			sample.last.xyz * basis8,
		0.0);
}

// the debug views of the clipmap: which clip lights a point (8: its color, DebugClipColor, blended where clips meet as
// GiSampleAt blends them; dark where none does), or (9) the indirect diffuse with the cells of the finest clip covering
// the point drawn over it: its voxels' thin, in the clip's color, and its probes' (the planes through them,
// where the probes interpolated change) thick, in white. on the planes across the surface only: one along it would
// cover it
float3 GiClipDebug(float3 position, float3 normal, bool grid)
{
	if (gGi[gPushConstants.frameIndex][0].enabled == 0)
		return -1.0;
	uint clipCount = gGi[gPushConstants.frameIndex][0].clipCount;
	float3 color = 0.0;
	float remaining = 1.0; // of the point, not yet the finer clips'
	uint finest = clipCount;
	for (uint clipIt = 0; clipIt < clipCount && remaining > 0.0; clipIt++)
	{
		GiClipView clip = GiClipViewAt(clipIt);
		float weight = GiClipWeight(clip, position);
		if (weight <= 0.0)
			continue;
		if (finest == clipCount)
			finest = clipIt;
		color += remaining * weight * DebugClipColor(clipIt);
		remaining *= 1.0 - weight;
	}
	if (!grid)
		return finest == clipCount ? float3(0.02) : color;

	GiSample sample = GiSampleAt(position, normal);
	float3 irradiance = sample.valid ? GiEvaluate(sample, normal) : -1.0;
	if (finest == clipCount)
		return irradiance;
	GiClipView clip = GiClipViewAt(finest);
	float3 coordinate = (position - clip.volumeOrigin.xyz) / clip.volumeOrigin.w;
	float3 across = step(abs(normal), 0.9); // the axes whose planes cross the surface
	float3 voxelLine = abs(frac(coordinate + 0.5) - 0.5); // voxels from the nearest voxel boundary
	float3 probeLine = abs(frac(coordinate / clip.probeSpacing) - 0.5) * clip.probeSpacing; // and from a probe's plane
	bool onProbe = any(select(probeLine < 0.08, across, 0.0) > 0.0);
	bool onVoxel = any(select(voxelLine < 0.03, across, 0.0) > 0.0);
	float3 base = max(irradiance, 0.0);
	if (onProbe)
		return float3(1.0);
	if (onVoxel)
		return lerp(base, DebugClipColor(finest), 0.6);
	return irradiance;
}

// the first voxel a ray from origin meets (level 0, each voxel it crosses visited once, as GiVisibility): through the
// finest clip holding the origin, then the coarser ones (into the last from outside it, where the origin is outside all).
// its clip and its index in gGiVoxels; false where it meets none
bool GiTraceVoxels(float3 origin, float3 direction, out uint hitClip, out uint hitIndex)
{
	hitClip = 0;
	hitIndex = 0;
	uint clipCount = gGi[gPushConstants.frameIndex][0].clipCount;
	float travelled = 0.0;
	uint stepCount = 0;
	for (uint clipIt = 0; clipIt < clipCount; clipIt++)
	{
		GiClipView clip = GiClipViewAt(clipIt);
		float3 size = float3(clip.volumeSize.xyz);
		float3 start = (origin + direction * travelled - clip.volumeOrigin.xyz) / clip.volumeOrigin.w;
		if (any(start < 0.0) || any(start >= size))
		{
			if (clipIt + 1 < clipCount)
				continue;
			// into the last clip from outside: where the ray enters its box, if it does
			float3 inverse = 1.0 / select(abs(direction) > 1e-8, direction, 1e-8);
			float3 low = (0.0 - start) * inverse;
			float3 high = (size - start) * inverse;
			float enter = max(max(min(low.x, high.x), min(low.y, high.y)), min(low.z, high.z));
			float leave = min(min(max(low.x, high.x), max(low.y, high.y)), max(low.z, high.z));
			if (enter > leave || leave < 0.0)
				return false;
			start = clamp(start + direction * (enter + 1e-3), 0.0, size - 1e-3);
		}
		int3 voxel = int3(floor(start));
		int3 stepDirection = int3(select(direction >= 0.0, 1, -1));
		float3 inverse = 1.0 / max(abs(direction), 1e-8);
		float3 next = (select(direction >= 0.0, float3(voxel + 1) - start, start - float3(voxel))) * inverse;
		float3 delta = inverse;
		float t = 0.0;
		while (GiInside(voxel, clip.volumeSize.xyz) && stepCount++ < 4096u)
		{
			uint index = clip.voxelFirst + GiVoxelIndex(uint3(voxel), clip.volumeSize.xyz);
			if ((gGiVoxels[index].albedo >> 24) != 0u)
			{
				hitClip = clipIt;
				hitIndex = index;
				return true;
			}
			if (next.x < next.y && next.x < next.z)
			{
				t = next.x;
				next.x += delta.x;
				voxel.x += stepDirection.x;
			}
			else if (next.y < next.z)
			{
				t = next.y;
				next.y += delta.y;
				voxel.y += stepDirection.y;
			}
			else
			{
				t = next.z;
				next.z += delta.z;
				voxel.z += stepDirection.z;
			}
		}
		travelled += t * clip.volumeOrigin.w;
	}
	return false;
}

// the debug views of the voxel world (DEBUG_VIEW_GI_VOXEL_WORLD*): a ray of a pixel's, from the eye, and what the first
// voxel it meets holds (its radiance as the rays read it, from the levels' level 0; its albedo; its normal); the
// environment where it meets none
float3 GiVoxelWorldDebug(float3 eye, float3 direction, uint debugView)
{
	uint clip;
	uint index;
	if (!GiTraceVoxels(eye, direction, clip, index))
		return debugView == DEBUG_VIEW_GI_VOXEL_WORLD ? EnvironmentRadiance(gEnvironment[0], direction, 0.0) : float3(0.05, 0.1, 0.2);
	GiVoxel voxel = gGiVoxels[index];
	if (debugView == DEBUG_VIEW_GI_VOXEL_WORLD_ALBEDO)
		return GiUnpackRgba8(voxel.albedo).rgb;
	if (debugView == DEBUG_VIEW_GI_VOXEL_WORLD_NORMAL)
	{
		float3 normal = GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0);
		// facing the eye or away from it (a voxel lit from behind what the eye sees): dimmed
		return pow((normal * 0.5 + 0.5) * (dot(normal, direction) < 0.0 ? 1.0 : 0.25), 2.2);
	}
	GiData gi = GiClip(clip);
	uint local = index - gi.voxelFirst;
	return GiUnpackRgba16(gGiRadiance[GiLevelFirst(gi, 0) + local]).rgb;
}

// the debug view of the probes' state (DEBUG_VIEW_GI_PROBE_STATE): of the 8 probes around the point of the finest clip
// that holds it, trilinear, the share marked inside something (red), the share whose depth map toward the point is empty
// (green) and the share failing the visibility test for it (blue), the point out along the normal as the resolve's is
float3 GiProbeStateDebug(float3 position, float3 normal)
{
	uint clipCount = gGi[gPushConstants.frameIndex][0].clipCount;
	for (uint clipIt = 0; clipIt < clipCount; clipIt++)
	{
		// the finest clip that lights the point (as the shading takes them: a clip's probes outside its box around the
		// eye are never looked up)
		if (GiClipWeight(GiClipViewAt(clipIt), position) <= 0.0)
			continue;
		GiData gi = GiClip(clipIt);
		float3 point = position + normal * max(0.2 * gi.probeOrigin.w, gi.volumeOrigin.w);
		float3 fraction;
		int3 base = GiProbeBase(gi, position, fraction);
		const uint kTexelCount = SHADER_TYPES_GI_DEPTH_SIDE * SHADER_TYPES_GI_DEPTH_SIDE;
		float3 result = 0.0;
		for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		{
			int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
			uint3 probe = uint3(clamp(base + offset, int3(0), int3(gi.probeSize.xyz) - 1));
			float3 weights = select(offset != 0, fraction, 1.0 - fraction);
			float weight = weights.x * weights.y * weights.z;
			uint probeIndex = gi.probeFirst + GiProbeIndex(probe, gi.probeSize);
			float3 probePosition = GiProbePosition(gi, probe, probeIndex);
			float inside = gGiIrradiance[probeIndex * SHADER_TYPES_GI_SH_COUNT + 5].w > 0.5 ? 1.0 : 0.0;
			float3 toProbe = probePosition - point;
			float distance = length(toProbe);
			GiDepthTaps taps = GiDepthTapsAt(-toProbe / max(distance, 1e-6));
			float2 moments = 0.0;
			for (uint tapIt = 0; tapIt < 4; tapIt++)
			{
				float2 tap = GiUnpackHalf2(gGiProbeDepth[gi.depthFirst + (probeIndex - gi.probeFirst) * kTexelCount + taps.texel[tapIt]]);
				moments += taps.weight[tapIt] * float2(abs(tap.x), tap.y);
			}
			float empty = moments.x < 1e-3 ? 1.0 : 0.0;
			float d = distance / gi.probeOrigin.w;
			float variance = max(moments.y - moments.x * moments.x, 0.0) + 1e-4;
			float visibility = 1.0;
			if (d > moments.x)
			{
				float delta = d - moments.x;
				visibility = variance / (variance + delta * delta);
				visibility *= visibility * visibility;
			}
			float failing = empty == 0.0 && inside == 0.0 && visibility < 0.5 ? 1.0 : 0.0;
			// of the probes on the point's open side only (the ones behind its surface fail as they should)
			if (dot(toProbe, normal) < 0.0)
				continue;
			result += weight * float3(inside, empty, failing);
		}
		// as a data view's value (shown as itself, see DebugViewIsData)
		return pow(result, 2.2);
	}
	return -1.0;
}

// the debug view of the probes (DEBUG_VIEW_GI_PROBES): the 8 probes around the point of the finest clip that holds it,
// trilinear, each its irradiance along the normal as GiEvaluate has it (the cosine lobe over pi)
float3 GiProbesDebug(float3 position, float3 normal)
{
	uint clipCount = gGi[gPushConstants.frameIndex][0].clipCount;
	for (uint clipIt = 0; clipIt < clipCount; clipIt++)
	{
		GiData gi = GiClip(clipIt);
		float3 coordinate = GiVoxelCoordinate(gi, position, 0);
		if (any(coordinate < 0.0) || any(coordinate >= float3(GiLevelSize(gi, 0))))
			continue;
		float3 fraction;
		int3 base = GiProbeBase(gi, position, fraction);
		float4 basis0 = float4(0.282095, 2.0 / 3.0 * 0.488603 * normal);
		float3 d = normal;
		float4 basis1 = 0.25 * float4(1.092548 * d.x * d.y, 1.092548 * d.y * d.z, 0.315392 * (3.0 * d.z * d.z - 1.0), 1.092548 * d.x * d.z);
		float basis8 = 0.25 * 0.546274 * (d.x * d.x - d.y * d.y);
		float3 result = 0.0;
		for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		{
			int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
			uint3 probe = uint3(clamp(base + offset, int3(0), int3(gi.probeSize.xyz) - 1));
			float3 weights = select(offset != 0, fraction, 1.0 - fraction);
			uint first = (gi.probeFirst + GiProbeIndex(probe, gi.probeSize)) * SHADER_TYPES_GI_SH_COUNT;
			float3 irradiance = 0.0;
			irradiance += gGiIrradiance[first + 0].rgb * basis0.x;
			irradiance += gGiIrradiance[first + 1].rgb * basis0.y;
			irradiance += gGiIrradiance[first + 2].rgb * basis0.z;
			irradiance += gGiIrradiance[first + 3].rgb * basis0.w;
			irradiance += gGiIrradiance[first + 4].rgb * basis1.x;
			irradiance += gGiIrradiance[first + 5].rgb * basis1.y;
			irradiance += gGiIrradiance[first + 6].rgb * basis1.z;
			irradiance += gGiIrradiance[first + 7].rgb * basis1.w;
			irradiance += gGiIrradiance[first + 8].rgb * basis8;
			result += weights.x * weights.y * weights.z * max(irradiance, 0.0);
		}
		return result;
	}
	return -1.0;
}
