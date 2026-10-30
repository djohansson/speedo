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

// a probe's depth texel (gGiProbeDepth) in a word: the mean distance as a half (negative while provisional, see
// ComputeGiProbeUpdate), the spread (the square root of the variance over the mean, up to 4) and the nearest the
// texel's rays met over the mean, 8 bits each: a second word (two halves) doubled what the resolve streams per block,
// every probe's 256 texels, for 2 to 3 ms a frame
uint GiPackDepth(float mean, float meanSquare, float nearest)
{
	float magnitude = max(abs(mean), 1e-6);
	float spread = sqrt(max(meanSquare - mean * mean, 0.0)) / magnitude;
	uint spreadBits = uint(round(saturate(spread * 0.25) * 255.0));
	uint nearestBits = uint(round(saturate(abs(nearest) / magnitude) * 255.0));
	return (f32tof16(mean) & 0xFFFFu) | (spreadBits << 16) | (nearestBits << 24);
}

// the mean (its sign kept), its square and the nearest (the mean's sign)
float3 GiUnpackDepth(uint v)
{
	float mean = f16tof32(v & 0xFFFFu);
	float magnitude = abs(mean);
	float spread = float((v >> 16) & 0xFFu) / 255.0 * 4.0 * magnitude;
	float nearest = float(v >> 24) / 255.0 * magnitude;
	return float3(mean, spread * spread + mean * mean, mean < 0.0 ? -nearest : nearest);
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
// a level's voxels per axis (the flags' blocks, SHADER_TYPES_GI_FLAG_LEVEL): rounded up, so that it covers all of the
// volume
uint3 GiLevelVoxels(uint3 size, uint level)
{
	return max((size + (1u << level) - 1u) >> level, uint3(1));
}

uint3 GiLevelSize(GiData gi, uint level)
{
	return GiLevelVoxels(gi.volumeSize.xyz, level);
}

// which of its triangles' faces wrote a voxel (see ComputeGiVoxelize): 1 the front's (it is behind the front face, and
// stands for the back side), 2 the back's (in front of it: the front side), 3 both
uint GiVoxelFaces(GiVoxel voxel)
{
	return (voxel.emissionB >> 16) & 3u;
}

// a double sided triangle wrote the voxel (GiVoxel::emissionB's bit 18)
static const uint kGiVoxelDoubleSided = 1u << 18;

// a voxel's normal (GiVoxel::normal: octahedral, x in 16 bits and y in 15, then whether a drawn side wrote it, see
// ComputeGiVoxelize)
static const uint kGiVoxelNormalDrawn = 1u << 31;

uint GiPackVoxelNormal(float3 normal, bool drawn)
{
	float2 octahedral = GiOctahedral(normal) * 0.5 + 0.5;
	return uint(round(octahedral.x * 65535.0)) | (uint(round(octahedral.y * 32767.0)) << 16) | (drawn ? kGiVoxelNormalDrawn : 0u);
}

float3 GiVoxelNormal(uint bits)
{
	return GiFromOctahedral(float2(float(bits & 0xFFFFu) / 65535.0, float((bits >> 16) & 0x7FFFu) / 32767.0) * 2.0 - 1.0);
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
	uint4 volumeImage; // see GiData::volumeImage
	uint voxelFirst;
	uint radianceFirst;
	uint flagFirst;
	uint frameStamp;
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
	view.volumeImage = gGi[gPushConstants.frameIndex][clip].volumeImage;
	view.voxelFirst = gGi[gPushConstants.frameIndex][clip].voxelFirst;
	view.radianceFirst = gGi[gPushConstants.frameIndex][clip].radianceFirst;
	view.flagFirst = gGi[gPushConstants.frameIndex][clip].flagFirst;
	view.frameStamp = gGi[gPushConstants.frameIndex][clip].frameStamp;
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

// a voxel's radiance (rgb) and whether something is in it (a), 0 outside the volume
float4 GiRadianceAt(GiData gi, int3 voxel)
{
	uint3 size = GiLevelSize(gi, 0);
	if (!GiInside(voxel, size))
		return 0.0;
	return GiUnpackRgba16(gGiRadiance[gi.radianceFirst + GiVoxelIndex(uint3(voxel), size)]);
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

// along a ray from origin, as far as end (world space): the radiance of the first voxel something is in, and whether it
// met none (a: 1 past the clips, the environment's share, else 0). each voxel the ray crosses is visited once
// (Amanatides and Woo), so no thin wall is stepped over or seen through. where it leaves a clip it goes on through the
// coarser ones (never back: they are boxes); a clip that follows the eye is left where its box around the eye is
// (GiData::fadeCenter), inside its own, so that where a ray goes on through the coarser clip's voxels (thicker) moves
// with the eye, smoothly, rather than with the clip's snaps. the rays used to march a mip chain of the radiance past
// two probe spacings (anisotropic, a level per four times the distance: a cone of a constant angle), through which a
// wall a voxel thick is half empty (a quarter at the level above), so every ray that met one kept a share of its
// transmittance and brought the sky in behind it: Sponza's arcades were lit from outside by a thousandth of the sky,
// the finest clip's probes (whose rays went coarse soonest) 4 times as much as the next clip's, so a dark band at its
// box's edge followed the camera along them. walking every voxel costs little: the rays end at the first voxel, the
// levels' kernel is gone with them, and a block (4x4x4 voxels, SHADER_TYPES_GI_FLAG_LEVEL) nothing is in this frame
// (its stamp in gGiFlags, which the lighting writes before the rays) is crossed in one step, so rays through the open
// (to the sky) cost by the blocks they cross (DamagedHelmet's frame 12.6 ms walking every voxel, Bistro's street 17.5).
// not for the references (skipEmptyBlocks): in the main pass's fragment shader, the surface reference's rays with the
// blocks' reads ran past Metal's command queue timeout (MTL_COMMAND_QUEUE_ERROR_TIMEOUT, VK_ERROR_DEVICE_LOST within
// seconds of starting, with three clips; one clip, or the walk alone, didn't), under validation too, which reported
// nothing else
// emissionReach: a probe's ray that meets an emitter nearer than it takes the emission as from that far away (scaled by
// the square of the distance over it, as a small source's irradiance falls), since what the probe holds lights the points
// of its cell, mostly about half a spacing from it, where a source beside the probe is that far: a street lamp's head
// beside one of Bistro's coarse probes (8.9 m apart) lit the square before the hotel's entrance and its facade yellow
// (the probe's faces 2.4 where its neighbors' were 0.06), where the surface reference, from the points themselves, has a
// pool under the lamp. instant radiosity clamps a virtual light's contribution the same way. 0 is no clamp (the references).
// where emissive surfaces are lights (see EmittersLight), none of their emission: what they give arrives through them
float4 GiMarch(GiData gi, float3 origin, float3 direction, float end, bool skipEmptyBlocks, float emissionReach = 0.0)
{
	if (EmittersLight())
		emissionReach = 1e30;
	float travelled = 0.0;
	const uint kBlockShift = SHADER_TYPES_GI_FLAG_LEVEL;
	for (uint clipIt = gi.clip; clipIt < gi.clipCount; clipIt++)
	{
		GiClipView clip = GiClipViewAt(clipIt);
		uint3 size = clip.volumeSize.xyz;
		uint3 blockCount = GiLevelVoxels(size, kBlockShift);
		float3 start = (origin + direction * travelled - clip.volumeOrigin.xyz) / clip.volumeOrigin.w + direction * 1e-3;
		int3 voxel = int3(floor(start));
		int3 stepDirection = int3(select(direction >= 0.0, 1, -1));
		float3 inverse = 1.0 / max(abs(direction), 1e-8);
		float3 next = (select(direction >= 0.0, float3(voxel + 1) - start, start - float3(voxel))) * inverse;
		float3 delta = inverse;
		float endHere = (end - travelled) / clip.volumeOrigin.w;
		float boxExit = GiFadeBoxExit(clip.fadeCenter, clip.fadeExtent, origin + direction * travelled, direction) / clip.volumeOrigin.w;
		float t = 0.0;
		bool left = false;
		int3 block = int3(-1);
		uint steps = 0;
		float passedT = -1e9;
		float3 passedRadiance = 0.0;
		while (t < endHere)
		{
			// (only where it skips: the writes made the main pass's fragment shader, which holds the surface reference's
			// walk, one with side effects, so its early depth test was off and Sponza's Main took 10 ms rather than 4)
			if (skipEmptyBlocks && ++steps > 4096u)
			{
				InterlockedAdd(gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_RAYS], 1u);
				for (uint axisIt = 0; axisIt < 3; axisIt++)
				{
					gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_ORIGIN + axisIt] = asuint(origin[axisIt]);
					gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_DIRECTION + axisIt] = asuint(direction[axisIt]);
					gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_STATE + 4 + axisIt] = asuint(float(voxel[axisIt]));
				}
				gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_STATE] = asuint(float(clipIt));
				gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_STATE + 1] = asuint(t);
				gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_STATE + 2] = asuint(endHere);
				gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_STATE + 3] = asuint(boxExit);
				gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_STATE + 7] = asuint(travelled);
				return float4(0.0, 0.0, 0.0, 0.0);
			}
			if (!GiInside(voxel, size) || t > boxExit)
			{
				left = true;
				break;
			}
			// a block entered that nothing is in: to just past where the ray leaves it, and the walk again from there (no
			// continue: with one, the block last read wasn't kept across it on KosmicKrisp, so a ray starting on a block's
			// face with a hair of its direction into it skipped the same nothing forever, and hung the gpu)
			int3 at = voxel >> kBlockShift;
			bool skipped = false;
			if (skipEmptyBlocks && any(at != block))
			{
				block = at;
				uint stamp = gGiFlags[clip.flagFirst + GiVoxelIndex(uint3(at), blockCount) * SHADER_TYPES_GI_FLAG_WORD_COUNT] >> SHADER_TYPES_GI_FLAG_STAMP_SHIFT;
				if (stamp != clip.frameStamp)
				{
					float3 bound = float3(select(direction >= 0.0, (at + 1) << kBlockShift, at << kBlockShift));
					float3 sign = float3(direction.x >= 0.0 ? 1.0 : -1.0, direction.y >= 0.0 ? 1.0 : -1.0, direction.z >= 0.0 ? 1.0 : -1.0);
					float3 exits = (bound - start) * inverse * sign;
					t = max(min(exits.x, min(exits.y, exits.z)), t) + 1e-3;
					float3 position = start + direction * t;
					voxel = int3(floor(position));
					next = (select(direction >= 0.0, float3(voxel + 1) - position, position - float3(voxel))) * inverse + t;
					skipped = true;
				}
			}
			if (!skipped)
			{
				uint index = GiVoxelIndex(uint3(voxel), size);
				float4 found = GiUnpackRgba16(gGiRadiance[clip.radianceFirst + index]);
				if (found.a > 0.0)
				{
					// the voxel a back face alone wrote is a surface's shell on its open side (see GiOccluderHit): passed, and
					// what the ray sees of the surface when the twin behind it is met
					if (GiVoxelFaces(gGiVoxels[clip.voxelFirst + index]) == 2u)
					{
						passedT = t;
						passedRadiance = found.rgb;
					}
					else if (t - passedT < 2.0)
						return float4(passedRadiance, 0.0);
					else
					{
						float3 radiance = found.rgb;
						float hit = travelled + t * clip.volumeOrigin.w;
						if (hit < emissionReach)
						{
							GiVoxel met = gGiVoxels[clip.voxelFirst + index];
							float3 emission = float3(GiUnpackHalf2(met.emissionRG), GiUnpackHalf2(met.emissionB).x);
							radiance = max(radiance - emission * (1.0 - (hit * hit) / (emissionReach * emissionReach)), 0.0);
						}
						return float4(radiance, 0.0);
					}
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
		}
		if (!left)
			break;
		travelled += t * clip.volumeOrigin.w;
	}
	return float4(0.0, 0.0, 0.0, 1.0);
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
// shell in the finer clip is inside the thicker shell of the coarser. a surface writes a voxel on each of its sides (see
// ComputeGiVoxelize); the one its back face alone wrote (faces 2: outside a solid, since the importers wind faces
// outward) is passed, and is the surface the ray met if the twin behind it is hit within two voxels (its normal, and
// GiMarch takes its radiance): so a surface is a voxel thick for the rays rather than two, and an opening a voxel wider
// a side. at 0.29 m voxels Sponza's first floor arcade lost its openings' lower 0.6 m to the shells, so no sun reached
// its floor in the last clip and no probe of it held light (the corridor black beyond the finer clips' boxes, the
// third artifact that followed the camera); the lookups and the relocation keep both shells
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
		float passedT = -1e9;
		uint passedNormal = 0;
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
				uint faces = GiVoxelFaces(found);
				if (faces == 2u)
				{
					passedT = t;
					passedNormal = found.normal;
				}
				else
				{
					uint normalBits = t - passedT < 2.0 ? passedNormal : found.normal;
					float3 normal = GiVoxelNormal(normalBits);
					backface = travelled + t > 0.0 && faces != 3u && dot(normal, direction) > 0.0;
					return travelled + t * clip.volumeOrigin.w;
				}
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

// whether the rays traverse the model's BVHs (GiData::trace, see bvh.slang) rather than walk the voxels: they meet the
// triangles themselves (no surface is a voxel thick, no opening narrowed by a voxel either side, no thin wall half
// crossed), and read the voxels' radiance where they hit (GiRadianceAtHit). what doesn't move is one BVH (gBvhNodes),
// what moves another, placed each frame (gBvhMovingNodes[frame], see gfx::bvh::Moving)
bool GiTracesBvh(GiData gi)
{
	return gi.trace == GI_TRACE_BVH;
}

// the BVH's alpha masked triangles cut out as the main pass's alpha test does (FragmentShadow): the base color's alpha or
// the mask's red below the material's cutoff, at the hit's texcoords (BVH_ALPHA_LEVEL), so that light passes through
// foliage's gaps (the voxels hold such a triangle whole)
struct GiAlphaMask : IBvhFilter
{
	bool Accept(uint flags, uint firstIndex, float2 barycentrics)
	{
		if ((flags & BVH_TRIANGLE_FLAG_ALPHA_MASKED) == 0u)
			return true;
		MaterialData material = gMaterialData[flags >> BVH_TRIANGLE_MATERIAL_SHIFT];
		float4 texCoord01 = gVertexBuffer[gIndexBuffer[firstIndex]].texCoord01 * (1.0 - barycentrics.x - barycentrics.y) +
							gVertexBuffer[gIndexBuffer[firstIndex + 1]].texCoord01 * barycentrics.x +
							gVertexBuffer[gIndexBuffer[firstIndex + 2]].texCoord01 * barycentrics.y;
		if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
		{
			TextureView view = gTextureViews[material.baseColorView];
			if (SampleViewLevel(view, ViewTexCoord(view, texCoord01), BVH_ALPHA_LEVEL).a < material.alphaCutoff)
				return false;
		}
		if ((material.flags & MATERIAL_FLAG_ALPHA_TEXTURE) != 0u)
		{
			TextureView view = gTextureViews[material.alphaView];
			if (SampleViewLevel(view, ViewTexCoord(view, texCoord01), BVH_ALPHA_LEVEL).r < material.alphaCutoff)
				return false;
		}
		return true;
	}
}

// what a ray from origin meets of the model's triangles before tmax (the nearest, or with anyHit any): the static BVH's,
// then with moving the moving BVH's, nearer than what it met. capped if either traversal hit its step cap, which with
// count (compute only: a write in the main pass's fragment shader, which the surface reference traces from, turned its
// early depth test off, see GiMarch) is counted (GI_STATISTIC_CAPPED_BVH_RAYS)
BvhHit GiTraceTriangles(GiData gi, float3 origin, float3 direction, float tmax, bool anyHit, bool moving, bool count)
{
#if BVH_RAY_QUERY
	return BvhTraceScene(origin, direction, tmax, anyHit, moving && gi.bvhMoving != 0u, GiAlphaMask());
#else
	BvhHit hit = BvhTraverse(gBvhNodes, gBvhTriangles, origin, direction, tmax, anyHit, GiAlphaMask());
	if (moving && gi.bvhMoving != 0u && !(anyHit && BvhHitSomething(hit)))
	{
		uint frameIndex = gPushConstants.frameIndex;
		BvhHit movingHit = BvhTraverse2(gBvhMovingNodes[frameIndex], gBvhMovingTriangles[frameIndex], origin, direction, hit.t, anyHit, GiAlphaMask());
		bool capped = hit.capped || movingHit.capped;
		if (BvhHitSomething(movingHit))
			hit = movingHit;
		hit.capped = capped;
	}
	if (count && hit.capped)
		InterlockedAdd(gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CAPPED_BVH_RAYS], 1u);
	return hit;
#endif
}

// whether the voxels let a ray through from origin, a distance along direction (a shadow ray): the BVHs' triangles (any
// hit) where the rays trace them, what moves with moving (not for the static voxels' kept light, which nothing moving
// may shadow for good: see ComputeGiLightStatic)
float GiVisibility(GiData gi, float3 origin, float3 direction, float distance, bool moving)
{
	if (GiTracesBvh(gi))
	{
		// a capped shadow ray is taken as shadowed: it was in something its traversal couldn't get through
		BvhHit hit = GiTraceTriangles(gi, origin, direction, distance, true, moving, true);
		return BvhHitSomething(hit) || hit.capped ? 0.0 : 1.0;
	}
	return GiOccluderDistance(gi, origin, direction, distance) < distance ? 0.0 : 1.0;
}

// the radiance where a ray met a triangle (position; normal: the triangle's, toward the side the ray came from), and the
// emission in it (out): the voxel half a voxel off the surface on that side, which stands for the surface as seen and lit
// from there (see ComputeGiVoxelize), else the voxel the point is in or the next one out (a voxel's samples needn't
// land where the hit is, at a thin or curved surface); 0 if all are empty. in the finest clip from firstClip whose box
// around the eye holds the point (the last holds everything), as the voxel walk reads a clip until it leaves that box
float3 GiRadianceAtHit(uint firstClip, float3 position, float3 normal, out float3 emission)
{
	emission = 0.0;
	uint clipCount = gGi[gPushConstants.frameIndex][firstClip].clipCount;
	uint clipIt = firstClip;
	for (; clipIt + 1 < clipCount; clipIt++)
	{
		GiClipView view = GiClipViewAt(clipIt);
		if (all(abs(position - view.fadeCenter.xyz) < view.fadeExtent.xyz))
			break;
	}
	GiClipView clip = GiClipViewAt(clipIt);
	uint3 size = clip.volumeSize.xyz;
	float3 coordinate = (position - clip.volumeOrigin.xyz) / clip.volumeOrigin.w;
	const float kOffsets[3] = {0.5, 0.0, 1.0};
	for (uint offsetIt = 0; offsetIt < 3; offsetIt++)
	{
		int3 voxel = int3(floor(coordinate + normal * kOffsets[offsetIt]));
		if (!GiInside(voxel, size))
			continue;
		uint index = GiVoxelIndex(uint3(voxel), size);
		float4 found = GiUnpackRgba16(gGiRadiance[clip.radianceFirst + index]);
		if (found.a > 0.0)
		{
			GiVoxel met = gGiVoxels[clip.voxelFirst + index];
			emission = float3(GiUnpackHalf2(met.emissionRG), GiUnpackHalf2(met.emissionB).x);
			return found.rgb;
		}
	}
	return 0.0;
}

// the radiance a ray took off the triangle it hit: the light its voxel reflects (GiRadianceAtHit, less the emission in
// it) and the triangle's own emission (its material's, times its emissive texture at the hit's texcoords). a voxel holds
// the most emission of what wrote it, so with the voxel's, a ray meeting anything in an emitter's voxel took all of it:
// Bistro's lanterns, a bulb of a few centimetres (3.5, nearly white) in voxels of 30 cm with their metal frames and
// glass, lit the walls around them yellow, a hundred times what the bulb gives, and in blotches, since only some of a
// wall's rays met a lantern. where the rays walk the voxels there is no triangle, and the voxel's stands. where emissive
// surfaces are lights (see EmittersLight), the reflected light alone: what they give arrives through them
float3 GiHitReflected(uint firstClip, float3 position, float3 normal)
{
	float3 voxelEmission;
	return max(GiRadianceAtHit(firstClip, position, normal, voxelEmission) - voxelEmission, 0.0);
}

float3 GiHitRadiance(uint firstClip, float3 position, BvhHit hit, out float3 emission)
{
	float3 reflected = GiHitReflected(firstClip, position, hit.normal);
	emission = 0.0;
	if (EmittersLight())
		return reflected;
	MaterialData material = gMaterialData[hit.flags >> BVH_TRIANGLE_MATERIAL_SHIFT];
	emission = material.emissive.rgb;
	if (any(emission > 0.0) && (material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
	{
		float2 weights = hit.barycentrics;
		float4 texCoord01 = gVertexBuffer[gIndexBuffer[hit.firstIndex]].texCoord01 * (1.0 - weights.x - weights.y) +
							gVertexBuffer[gIndexBuffer[hit.firstIndex + 1]].texCoord01 * weights.x +
							gVertexBuffer[gIndexBuffer[hit.firstIndex + 2]].texCoord01 * weights.y;
		TextureView view = gTextureViews[material.emissiveView];
		emission *= SampleViewLevel(view, ViewTexCoord(view, texCoord01), BVH_ALPHA_LEVEL).rgb;
	}
	return reflected + emission;
}

// what a ray from origin meets through the BVHs (see GiTracesBvh), as far as far: its radiance (rgb, the voxels' where it
// hits, see GiRadianceAtHit and GiMarch's emissionReach) and whether it met nothing (a: the environment's share), how
// far it went, and whether what it met was a single sided surface seen from behind (as GiOccluderHit's backface: the ray
// came from inside what the surface bounds). false where the rays walk the voxels instead
struct GiRayHit
{
	float4 radiance;
	float distance;
	bool backface;
};

bool GiTraceBvh(GiData walk, float3 origin, float3 direction, float far, float emissionReach, out GiRayHit result, bool count = true)
{
	result.radiance = float4(0.0, 0.0, 0.0, 1.0);
	result.distance = far;
	result.backface = false;
	if (!GiTracesBvh(walk))
		return false;
	BvhHit hit = GiTraceTriangles(walk, origin, direction, far, false, true, count);
	if (!BvhHitSomething(hit))
		return true;
	result.distance = hit.t;
	float3 emission;
	float3 radiance = GiHitRadiance(walk.clip, origin + direction * hit.t, hit, emission);
	if (hit.t < emissionReach)
		radiance = max(radiance - emission * (1.0 - (hit.t * hit.t) / (emissionReach * emissionReach)), 0.0);
	result.radiance = float4(radiance, 0.0);
	result.backface = !hit.frontFace && (hit.flags & BVH_TRIANGLE_FLAG_DOUBLE_SIDED) == 0u;
	return true;
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

// the most rows of samples a triangle is voxelized by (see ComputeGiVoxelize): half a voxel apart, a triangle up to 256
// voxels long; a longer one is rasterized (GiVoxelizeRasterize)
static const uint kGiVoxelizeSampledSteps = 512u;

// a voxelized triangle's side written into a voxel (see ComputeGiVoxelize): what doesn't move only where it scrolled in
// (the rest is shifted, see ComputeGiShift), what moves everywhere
void GiVoxelizeWrite(GiData gi, uint3 size, int3 at, GiVoxel voxel, uint sideIt, bool doubleSided, bool moving)
{
	if (!GiInside(at, size) || (!moving && !GiScrolledIn(gi, at)))
		return;
	uint index = gi.voxelFirst + GiVoxelIndex(uint3(at), size);
	// something moving in this block this frame (see gGiFlags' second word; a plain store of the same)
	if (moving)
		gGiFlags[GiFlagIndex(gi, GiVoxelIndex(uint3(at) >> SHADER_TYPES_GI_FLAG_LEVEL, GiLevelSize(gi, SHADER_TYPES_GI_FLAG_LEVEL))) + 1u] = gi.frameStamp;
	InterlockedMax(gGiVoxels[index].albedo, voxel.albedo);
	InterlockedMax(gGiVoxels[index].normal, voxel.normal);
	InterlockedMax(gGiVoxels[index].emissionRG, voxel.emissionRG);
	InterlockedMax(gGiVoxels[index].emissionB, voxel.emissionB);
	// and which of the triangle's faces wrote it (bits 16 and 17): both, the voxel is a thin wall's, whose normal is
	// either face's (see GiOccluderHit); and whether a double sided one did (bit 18, see kGiVoxelDoubleSided)
	InterlockedOr(gGiVoxels[index].emissionB, ((sideIt + 1u) << 16) | (doubleSided ? kGiVoxelDoubleSided : 0u));
}

// a long triangle's side (q0, q1, q2: its corners in voxel coordinates, half a voxel behind the face) voxelized by
// rasterizing it along its normal's dominant axis: each column of voxels along that axis whose square the triangle's
// projection overlaps (the edges' functions at the square's corner farthest inside, with the bounds: exact, so a sliver
// misses none), the voxels its plane crosses over the square (within the triangle's extent along the axis), within
// the box (the clip, or the slab that scrolled in). a voxel column an iteration, where sampling half a voxel apart took
// about four samples a voxel: voxelizing at resolution 384 when a clip scrolled, Sponza's (its corridor floor's
// triangles some 10 m long) took 4.4 ms sampled at most 512 rows apart (with the holes), 13 sampled half a voxel apart,
// 3.4 rasterized; Bistro's street 3.5 and 3.0. the triangles that need no more rows are sampled as they were
void GiVoxelizeRasterize(GiData gi, uint3 size, float3 q0, float3 q1, float3 q2, float3 boxLow, float3 boxHigh, GiVoxel voxel, uint sideIt, bool doubleSided, bool moving)
{
	float3 n = cross(q1 - q0, q2 - q0);
	float3 m = abs(n);
	uint d = m.x >= m.y && m.x >= m.z ? 0u : (m.y >= m.z ? 1u : 2u);
	if (m[d] <= 0.0)
		return;
	uint u = (d + 1u) % 3u;
	uint w = (d + 2u) % 3u;
	// the projection, counter-clockwise, so that inside is where no edge's function is below 0
	float2 corners[3] = {float2(q0[u], q0[w]), float2(q1[u], q1[w]), float2(q2[u], q2[w])};
	float2 ab = corners[1] - corners[0];
	float2 ac = corners[2] - corners[0];
	if (ab.x * ac.y - ab.y * ac.x < 0.0)
	{
		float2 corner = corners[1];
		corners[1] = corners[2];
		corners[2] = corner;
	}
	// the plane along d over a square: its depth at the center, give or take slope
	float plane = dot(n, q0);
	float slope = 0.5 * (m[u] + m[w]) / m[d];
	float depthLow = max(min(q0[d], min(q1[d], q2[d])), boxLow[d]);
	float depthHigh = min(max(q0[d], max(q1[d], q2[d])), boxHigh[d]);
	float2 projectedLow = min(corners[0], min(corners[1], corners[2]));
	float2 projectedHigh = max(corners[0], max(corners[1], corners[2]));
	int2 low = int2(floor(max(projectedLow, float2(boxLow[u], boxLow[w]))));
	int2 high = int2(floor(min(projectedHigh, float2(boxHigh[u], boxHigh[w]))));
	for (int cu = low.x; cu <= high.x; cu++)
	{
		for (int cw = low.y; cw <= high.y; cw++)
		{
			float2 center = float2(float(cu), float(cw)) + 0.5;
			bool overlaps = true;
			for (uint edgeIt = 0; edgeIt < 3u; edgeIt++)
			{
				float2 from = corners[edgeIt];
				float2 edge = corners[(edgeIt + 1u) % 3u] - from;
				float function = edge.x * (center.y - from.y) - edge.y * (center.x - from.x);
				overlaps = overlaps && function + 0.5 * (abs(edge.x) + abs(edge.y)) >= 0.0;
			}
			if (!overlaps)
				continue;
			float depth = (plane - n[u] * center.x - n[w] * center.y) / n[d];
			int first = int(floor(max(depth - slope, depthLow)));
			int last = int(floor(min(depth + slope, depthHigh)));
			for (int k = first; k <= last; k++)
			{
				int3 at = 0;
				at[d] = k;
				at[u] = cu;
				at[w] = cw;
				GiVoxelizeWrite(gi, size, at, voxel, sideIt, doubleSided, moving);
			}
		}
	}
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
	uint instanceCount = job.w & 0x3FFFFFFFu;
	bool moving = (job.w >> 31) != 0u; // a job of what moves (see Renderer::InternalPlanGi)
	bool doubleSided = ((job.w >> 30) & 1u) != 0u;
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
				color *= SampleViewLevel(view, ViewTexCoord(view, texCoord01), 6.0);
			}
			// metallic is the factor times the texture's (its green, see image::Usage::kMetallicRoughness), as the shading
			// has it: by the factor alone (1 by default, the texture saying where), Sponza's voxels were all black
			float metallic = material.metallic;
			if ((material.flags & MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE) != 0u)
			{
				TextureView view = gTextureViews[material.metallicRoughnessView];
				metallic *= SampleViewLevel(view, ViewTexCoord(view, texCoord01), 6.0).g;
			}
			albedo = color.rgb * (1.0 - metallic);
			emission = material.emissive.rgb;
			if ((material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
			{
				TextureView view = gTextureViews[material.emissiveView];
				emission *= SampleViewLevel(view, ViewTexCoord(view, texCoord01), 6.0).rgb;
			}
		}
		// sampled half a voxel apart along its longest edge, in rows of samples, up to kGiVoxelizeSampledSteps of them: a
		// longer triangle is rasterized instead (GiVoxelizeRasterize). sampled at most 512 rows apart, as it was, a triangle
		// longer than 256 voxels was sampled a little more than a voxel apart and skipped voxel columns now and then:
		// Sponza's corridor floor, its triangles some 10 m long, had holes in lines across it at resolution 384 (voxels of
		// 2 cm in the finest clip), where the surface cache's entries saw through it and drew bright lines along the floor.
		// sampled half a voxel apart however long, a long triangle a thread, Sponza's voxelizing at 384 took 13 ms rather
		// than 4.4 whenever a clip scrolled
		float longest = max(length(p1 - p0), max(length(p2 - p1), length(p0 - p2)));
		float needed = ceil(longest / (gi.volumeOrigin.w * 0.5));
		bool rasterize = needed > float(kGiVoxelizeSampledSteps);
		uint steps = clamp(uint(needed), 1u, kGiVoxelizeSampledSteps);
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
			// a drawn side's normal wins (the atomic maximum below, by its top bit): the front side's (behind the back face),
			// either of a double sided triangle's. a voxel written by one triangle's front side and another's back, at a
			// single sided ceiling's edge on a wall (the wall's room side and the space over the ceiling), took either
			// normal, and was resolved as the space over the ceiling when it took the ceiling's: lit from the sky through
			// it, it drew a bright line along salle_de_bain's walls' tops
			GiVoxel voxel;
			voxel.albedo = GiPackRgba8(float4(albedo, 1.0));
			voxel.normal = GiPackVoxelNormal(-facing, sideIt == 1u || doubleSided);
			// the emission into the voxel in front of the front face only (an emitter emits from its front): into both, an
			// interior's emissive ceiling, which meets or runs through the facade, lit the facade's own voxels at coarse
			// voxels (the yellow around the entrance by Bistro's square, seen from the street)
			float3 sideEmission = sideIt == 0 ? emission : 0.0;
			voxel.emissionRG = GiPackHalf2(sideEmission.rg);
			voxel.emissionB = GiPackHalf2(float2(sideEmission.b, 0.0));
			if (rasterize)
			{
				float3 shift = facing * (offset * gi.volumeOrigin.w);
				GiVoxelizeRasterize(gi, size, GiVoxelCoordinate(gi, p0 - shift, 0), GiVoxelCoordinate(gi, p1 - shift, 0), GiVoxelCoordinate(gi, p2 - shift, 0),
									boxLow, boxHigh, voxel, sideIt, doubleSided, moving);
				continue;
			}
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
					GiVoxelizeWrite(gi, size, at, voxel, sideIt, doubleSided, moving);
				}
			}
		}
	}
}

// how far a surface's voxel is open along its normal: from its center to a surface facing back at it there (a ray through
// the static BVH, as GiCacheOrigin's), at most kGiOpenReach voxels (all of it where the rays walk the voxels). what pushes
// off a voxel along its normal goes no further than halfway (its shadow rays' start, its bounce's lookup, the point its
// probes are tested from): a voxel facing a surface within a voxel (the slot behind salle_de_bain's bench, thinner than
// a last clip's voxel: the bench back's voxel faces the wall) reached past it. kept with the static voxels' direct light
// (its alpha, in voxels: see ComputeGiLightStatic)
static const float kGiOpenReach = 3.0;

float GiOpenAlong(GiData gi, float3 position, float3 normal)
{
	float reach = kGiOpenReach * gi.volumeOrigin.w;
	if (!GiTracesBvh(gi))
		return reach;
	BvhHit hit = GiTraceTriangles(gi, position, normal, reach, false, false, false);
	return BvhHitSomething(hit) && (hit.frontFace || (hit.flags & BVH_TRIANGLE_FLAG_DOUBLE_SIDED) != 0u) ? hit.t : reach;
}

// the shadow rays of a voxel's light start a voxel further from the surface (the voxel is on the side it stands for, its
// twin across it), but no further than halfway to a surface facing back at it: the bench back's voxel in the slot behind
// salle_de_bain's bench started them past the wall, outdoors, where the sun lit it, and whatever read it (the cache's rays
// in the slot, the probes', the near field's) lit the slot. open: how far it is open along its normal (see GiOpenAlong)
float GiLightPush(GiData gi, float open)
{
	return min(gi.volumeOrigin.w, 0.5 * open);
}

// the emissive surfaces' light off a voxel where they are lights (see EmitterVoxelIrradiance): its albedo over pi times
// it, from where its shadow rays start (GiLightPush), by numbers of the voxel's own
float3 GiEmitterRadiance(GiData gi, uint3 at, GiVoxel voxel, float4 albedo, bool moving, float open)
{
	if (!EmittersLight())
		return 0.0;
	float3 position = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	float3 normal = GiVoxelNormal(voxel.normal);
	float3 irradiance = EmitterVoxelIrradiance(
		gi, position + normal * GiLightPush(gi, open), normal, moving, uint2(GiVoxelIndex(at, GiLevelSize(gi, 0)), gi.clip));
	return albedo.rgb / 3.14159265 * irradiance;
}

// the lights' light off a voxel (at, of a clip): its emission plus its albedo over pi times the light reaching it from
// the lights of its cell, each shadowed by a ray through the voxels (the shadow atlas only covers what the camera sees),
// and with emitters, from the emissive surfaces where they are lights (GiEmitterRadiance; whose readers then take its
// emission out). open: how far it is open along its normal (see GiOpenAlong)
float3 GiDirectRadiance(GiData gi, uint3 at, GiVoxel voxel, float4 albedo, bool moving, float open, bool emitters)
{
	uint frameIndex = gPushConstants.frameIndex;
	uint3 size = GiLevelSize(gi, 0);
	float3 position = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	float3 normal = GiVoxelNormal(voxel.normal);
	float3 emission = float3(GiUnpackHalf2(voxel.emissionRG), GiUnpackHalf2(voxel.emissionB).x);

	uint3 cell = min(uint3(float3(at) / float3(size) * float(SHADER_TYPES_GI_LIGHT_GRID)), uint3(SHADER_TYPES_GI_LIGHT_GRID - 1));
	uint2 range = gGiLightCells[frameIndex][gi.lightCellFirst + (cell.z * SHADER_TYPES_GI_LIGHT_GRID + cell.y) * SHADER_TYPES_GI_LIGHT_GRID + cell.x];
	float3 irradiance = 0.0;
	float push = GiLightPush(gi, open);
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
		float visibility = GiVisibility(gi, position + normal * push, toLight, distance - push, moving);
		irradiance += light.intensity.rgb * attenuation * nDotL * visibility;
	}
	return emission + albedo.rgb / 3.14159265 * irradiance + (emitters ? GiEmitterRadiance(gi, at, voxel, albedo, moving, open) : 0.0);
}

// the static voxels' direct radiance (GiData::directFirst), when the clip is voxelized or the lights change: before what
// moves is voxelized over them, so that nothing moving shadows it for good. the part the lights don't change (the
// emitters' light, and how far each voxel is open along its normal, see GiOpenAlong: a ray once a voxelizing) is kept
// (GiData::keptFirst) and taken as it is where the lights changed alone (GiData::lightsOnly): with the emitters' samples
// and shadow rays every time, the sky's sun moving every frame cost Bistro's street 72 ms a frame, 13 without them
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
	float3 kept = 0.0;
	// how far it is open, in voxels in the alpha of both
	float open = kGiOpenReach * gi.volumeOrigin.w;
	if (albedo.a != 0.0)
	{
		uint3 at = uint3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
		if (gi.lightsOnly != 0u)
		{
			float4 keptValue = GiUnpackRgba16(gGiRadiance[gi.keptFirst + threadId.x]);
			kept = keptValue.rgb;
			open = keptValue.a * gi.volumeOrigin.w;
		}
		else
		{
			open = GiOpenAlong(gi, gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w, GiVoxelNormal(voxel.normal));
			kept = GiEmitterRadiance(gi, at, voxel, albedo, false, open);
		}
		radiance = GiDirectRadiance(gi, at, voxel, albedo, false, open, false) + kept;
	}
	if (gi.lightsOnly == 0u)
		gGiRadiance[gi.keptFirst + threadId.x] = GiPackRgba16(float4(kept, open / gi.volumeOrigin.w));
	gGiRadiance[gi.directFirst + threadId.x] = GiPackRgba16(float4(radiance, open / gi.volumeOrigin.w));
	// and the frame's radiance from nothing: the frame's lighting skips the blocks nothing is near, whose voxels must hold 0
	gGiRadiance[gi.radianceFirst + threadId.x] = uint2(0, 0);
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
		gGiRadiance[gi.radianceFirst + threadId.x] = uint2(0, 0);
		return;
	}
	// its block holds something this frame (see gGiFlags): a plain store, every voxel of the block storing the same (an
	// atomic maximum kept whatever a new buffer held, and a block it said was stamped in some far frame never counted)
	gGiFlags[flagIndex] = gi.frameStamp << SHADER_TYPES_GI_FLAG_STAMP_SHIFT;
	GiVoxel kept = gGiStaticVoxels[index];
	bool still = kept.albedo == voxel.albedo && kept.normal == voxel.normal;
	float4 direct = GiUnpackRgba16(gGiRadiance[gi.directFirst + threadId.x]);
	// how far it is open along its normal (see GiOpenAlong): a static voxel's as kept, a moving one's as far as it reaches
	// (not traced a frame: what moves is rarely in a slot thinner than a voxel)
	float open = still ? direct.a * gi.volumeOrigin.w : kGiOpenReach * gi.volumeOrigin.w;
	float3 radiance;
	// the kept light, unless a moving block lies between the voxel and a light that lights it (its shadow)
	if (still && !(gi.moving != 0u && GiMovingShadows(gi, at, voxel)))
		radiance = direct.rgb;
	else
		radiance = GiDirectRadiance(gi, at, voxel, albedo, true, open, true);
	// not on a reset: the volumes hold nothing yet. the probes' light 1.5 voxels out along its normal (see
	// GiIrradianceResolved), but no further than halfway to a surface facing back at it (the slot's bench back's voxel
	// took the light outdoors, past the wall)
	if (gi.bounce != 0u && gi.reset == 0u)
	{
		float3 position = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
		float3 normal = GiVoxelNormal(voxel.normal);
		GiSample sample = GiSampleClip(GiClipViewAt(gi.clip), position, normal, normal * min(1.5, 0.5 * open / gi.volumeOrigin.w));
		float3 bounce = sample.valid ? GiEvaluate(sample, normal) : -1.0;
		if (!any(isnan(bounce)) && !any(isinf(bounce)))
			radiance += albedo.rgb * max(bounce, 0.0);
	}
	gGiRadiance[gi.radianceFirst + threadId.x] = GiPackRgba16(float4(radiance, 1.0));
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
	float3 normal = GiVoxelNormal(voxel.normal);
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

// a probe's offset from its place in the lattice (DDGI's relocation, see ComputeGiProbeUpdate), kept with its irradiance
// (faces 2 to 4's w) at a probe index of either half (see GiData::probeFirst), or none for a probe new to the
// clip (kGiNoProbe)
static const uint kGiNoProbe = 0xFFFFFFFFu;

// a probe is inside something while more than this share of its rays meet a surface's back, as blended over its frames
// (its irradiance's face 5's w, see ComputeGiProbeUpdate)
static const float kGiInsideShare = 0.25;

// where the rays trace the BVHs, the resolve keeps per voxel whether each of its 8 probes sees it (gGiVisibility: 2 bits a
// probe, kGiVisible, kGiHidden or kGiBehind, and kGiVisibilityValid), traced again where a probe moved or the kept bits
// aren't valid (new to the clip, or after a reset), carried over from the last frame otherwise
static const uint kGiVisible = 0u; // nothing between them
static const uint kGiHidden = 1u; // a surface's front between them
static const uint kGiBehind = 2u; // a surface's back between them: the probe is behind it
static const uint kGiVisibilityValid = 1u << 16;

float3 GiProbeOffset(uint offsetIndex)
{
	if (offsetIndex == kGiNoProbe)
		return 0.0;
	uint first = offsetIndex * SHADER_TYPES_GI_IRRADIANCE_COUNT;
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
			away += GiVoxelNormal(found.normal);
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

// a probe's phase of the frame (see GiData::rayPhase): the frame's, plus its own start, hashed from its place in the
// clipmap's lattice (the same wherever its clip has scrolled to). with every probe at the frame's phase, the blend's
// ripple (the latest subset weighs 3%, the oldest of the 32 1.2%, so what a probe holds pulses with the cycle by the
// subsets' differences) was the same across the probes of a region, which pulsed together, a 32 frame flicker of
// Sponza's walls under auto exposure; at their own phases the ripples of a cell's 8 probes cancel in the lookup
uint GiProbePhase(GiData gi, uint3 probe)
{
	float3 anchor = gGi[gPushConstants.frameIndex][gi.clipCount - 1].probeOrigin.xyz;
	int3 lattice = int3(round((gi.probeOrigin.xyz - anchor) / gi.probeOrigin.w)) + int3(probe);
	uint hash = uint(lattice.x) * 73856093u ^ uint(lattice.y) * 19349663u ^ uint(lattice.z) * 83492791u;
	hash ^= hash >> 16;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;
	return (gi.rayPhase + hash) % gi.rayPhaseCount;
}

// a ray of a phase: a Fibonacci sphere of rayCount directions turned by the phase (a quaternion from a quasirandom
// sequence over the phases, Roberts' R3, Shoemake's rotation: every phase's set is as even as the Fibonacci sphere,
// where every 32nd point of one 32 times as big was a coarse spiral of its own, and the phases' light differed by a
// quarter, which the blending showed as a 32 frame pulse of 5%). the set is the same for every probe (each probe
// turning the set its own way made each probe's error its own, which showed as dots where the resolve's weights single
// a probe out; the same directions everywhere, the error is shared by the probes near each other, smooth)
float3 GiRayDirection(GiData gi, uint phase, uint rayIndex)
{
	float z = 1.0 - (float(rayIndex) + 0.5) * 2.0 / float(gi.rayCount);
	float radius = sqrt(max(1.0 - z * z, 0.0));
	float phi = float(rayIndex) * 2.39996323;
	float3 d = float3(radius * cos(phi), radius * sin(phi), z);
	float3 u = frac(0.5 + float(phase) * float3(0.8191725, 0.6710436, 0.5497005));
	float a = u.y * 6.28318531;
	float b = u.z * 6.28318531;
	float4 q = float4(sqrt(1.0 - u.x) * sin(a), sqrt(1.0 - u.x) * cos(a), sqrt(u.x) * sin(b), sqrt(u.x) * cos(b));
	return d + 2.0 * cross(q.xyz, cross(q.xyz, d) + q.w * d);
}

// a ray's share of the irradiance along each axis, +x, -x, +y, -y, +z, -z (an ambient cube, Valve's): the cosine of
// its angle to the axis, where it faces it. summed over a probe's rays times 4 / their count, each face is the
// irradiance over pi a surface facing that way gets (a uniform radiance gives it on every face). it replaced
// spherical harmonics of bands 0 to 2 (2026-10-21): their cosine lobe gave a bright compact source at a grazing angle
// 3/32 of its strength (the bands' 1/4 + cos / 2 + 5/16 P2(cos) at 90 degrees), and Bistro's lit interiors, a hundred
// times the sky, lit the square before their entrances yellow where the reference had a thin spill; a face gives a
// grazing source nothing, at the cost of smooth light's accuracy between the axes (a uniform sky from above gives a
// 45 degree slope 0.75 of its zenith irradiance where 0.854 is right; windowing the harmonics couldn't help, since the
// leak is their band 0's quarter that only the higher bands cancel)
void GiIrradianceBasis(float3 d, out float basis[SHADER_TYPES_GI_IRRADIANCE_COUNT])
{
	basis[0] = max(d.x, 0.0);
	basis[1] = max(-d.x, 0.0);
	basis[2] = max(d.y, 0.0);
	basis[3] = max(-d.y, 0.0);
	basis[4] = max(d.z, 0.0);
	basis[5] = max(-d.z, 0.0);
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

// the finest clip whose box around the eye holds a point, at most clip: a coarser clip's probe standing inside a finer
// clip walks its rays through the finer voxels first (the walks go on through the coarser clips), so that the clips'
// probes agree where they overlap. by their own voxels, the finer clip's sun patches lit its probes and not the coarser
// clip's beside them (its shells close more of the openings), a step at the finest clip's box edge, darker beyond,
// that moved with the eye (Sponza's first floor vault: 12% in the reference and the probes alike)
uint GiFinestClipHolding(float3 position, uint clip)
{
	for (uint clipIt = 0; clipIt < clip; clipIt++)
	{
		GiClipView view = GiClipViewAt(clipIt);
		if (all(abs(position - view.fadeCenter.xyz) < view.fadeExtent.xyz))
			return clipIt;
	}
	return clip;
}

// the probes' rays (a thread per probe and ray): each to the first voxel it meets (see GiMarch), as far as the clipmap
// reaches, the environment past it; and to the first voxel it meets by the depth's rules (see GiOccluderHit), as far as
// a probe's depth map tells (SHADER_TYPES_GI_DEPTH_REACH spacings); both from the finest clip holding the probe (see
// GiFinestClipHolding)
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
	float3 direction = GiRayDirection(gi, GiProbePhase(gi, probe), rayIndex);
	GiData last = GiClip(gi.clipCount - 1);
	float far = length(float3(last.volumeSize.xyz)) * last.volumeOrigin.w;
	GiData walk = GiClip(GiFinestClipHolding(origin, gi.clip));
	// and the depth, negative where what it met was a surface's back (see GiOccluderHit): through the BVH one query
	// answers both
	float reach = SHADER_TYPES_GI_DEPTH_REACH * gi.probeOrigin.w;
	float4 traced;
	bool backface;
	float depth;
	GiRayHit hit;
	if (GiTraceBvh(walk, origin, direction, far, gi.emissionReach * gi.probeOrigin.w, hit))
	{
		traced = hit.radiance;
		backface = hit.backface && hit.distance < reach;
		depth = min(hit.distance, reach) / gi.probeOrigin.w;
	}
	else
	{
		traced = GiMarch(walk, origin, direction, far, true, gi.emissionReach * gi.probeOrigin.w);
		depth = GiOccluderHit(walk, origin, direction, reach, false, backface) / gi.probeOrigin.w;
	}
	float3 radiance = traced.rgb + traced.a * EnvironmentRadiance(gEnvironment[0], direction, 0.6);
	// a ray that isn't finite (a NaN anywhere poisons every probe it reaches, and through the bounce every voxel, for good:
	// lerp(NaN, x, 1) is NaN) holds nothing
	if (any(isnan(radiance)) || any(isinf(radiance)) || isnan(depth) || isinf(depth))
		radiance = 0.0;
	gGiRays[gi.rayFirst + threadId.x] = GiPackRgba16(float4(radiance, backface ? -depth : depth));
}

// the probes (a group per probe, a thread per texel of its depth map): each blends this frame's rays into what it held
// last frame (the probe at its place then, shifted by the clip's scroll; a probe new to the clip starts from the coarser
// clip's light where it is, the last clip's from its rays alone): its irradiance along the axes (the first
// texel's thread: every ray weighed by its share of the sphere) and each texel of its depth map (the rays near the
// texel's direction, weighed by a power of the cosine of their angle to it; a texel no ray is near keeps what it had),
// by GiData::hysteresis, or by the frames it has accumulated while fewer. with debug 6, the reference: the irradiance
// from 512 rays through the voxels straight to the environment (seconds a frame), nothing blended
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
	uint phase = GiProbePhase(gi, probe);
	// a probe nothing is voxelized near holds nothing (its rays weren't traced, see ComputeGiTrace): no light, and a
	// depth of 0, which no point passes
	uint first = (gi.probeFirst + probeIndex) * SHADER_TYPES_GI_IRRADIANCE_COUNT;
	if ((GiFlagsAt(gi, GiProbeLatticePosition(gi, probe)) & 8u) == 0u)
	{
		for (uint texelIt = 0; texelIt < kTexelsPerThread; texelIt++)
			gGiProbeDepth[gi.depthFirst + probeIndex * kTexelCount + texelIndex + texelIt * SHADER_TYPES_GI_GROUP_SIZE] = 0u;
		if (texelIndex == 0u)
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
			{
				gGiIrradiance[first + shIt] = 0.0;
				// and its cycle (see GiData::cycleOffset)
				gGiIrradiance[first + gi.cycleOffset * SHADER_TYPES_GI_IRRADIANCE_COUNT + shIt] = 0.0;
				gGiIrradiance[first + 2 * gi.cycleOffset * SHADER_TYPES_GI_IRRADIANCE_COUNT + shIt] = 0.0;
			}
		return;
	}

	// what it held: the probe at its place last frame, if it was in the clip (and the clip wasn't reset)
	uint history = GiProbeHistory(gi, probe);
	bool shifted = history != kGiNoProbe;
	uint fromIndex = shifted ? history - gi.probePreviousFirst : 0u;
	float age = shifted ? gGiIrradiance[(gi.probePreviousFirst + fromIndex) * SHADER_TYPES_GI_IRRADIANCE_COUNT].w : 0.0;
	// and how much its frames' light differs from what it holds, frame to frame (face 1's w: its noise)
	float noise = shifted ? gGiIrradiance[(gi.probePreviousFirst + fromIndex) * SHADER_TYPES_GI_IRRADIANCE_COUNT + 1].w : 0.0;

	// one more probe traced this frame (see GI_STATISTIC_ACTIVE_PROBES)
	if (texelIndex == 0u)
		InterlockedAdd(gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_ACTIVE_PROBES], 1u);
	// the rays: the thread's texels' depth (texelIndex, and every group's worth of texels after it), and (the first
	// thread) the irradiance faces
	float3 texelDirection[kTexelsPerThread];
	float2 depthSum[kTexelsPerThread];
	float depthWeight[kTexelsPerThread];
	float2 wideSum[kTexelsPerThread];
	float wideWeight[kTexelsPerThread];
	float depthMin[kTexelsPerThread];
	float wideMin[kTexelsPerThread];
	for (uint texelIt = 0; texelIt < kTexelsPerThread; texelIt++)
	{
		uint texel = texelIndex + texelIt * SHADER_TYPES_GI_GROUP_SIZE;
		texelDirection[texelIt] = GiDirection(uint2(texel % SHADER_TYPES_GI_DEPTH_SIDE, texel / SHADER_TYPES_GI_DEPTH_SIDE), SHADER_TYPES_GI_DEPTH_SIDE);
		depthSum[texelIt] = 0.0;
		depthWeight[texelIt] = 0.0;
		wideSum[texelIt] = 0.0;
		wideWeight[texelIt] = 0.0;
		depthMin[texelIt] = 1e9;
		wideMin[texelIt] = 1e9;
	}
	float3 sh[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
		sh[shIt] = 0.0;
	float basis[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	uint backfaceCount = 0;
	float closestBackface = 1e30;
	float3 closestBackfaceDirection = 0.0;
	uint rayFirst = gi.rayFirst + probeIndex * gi.rayCount;
	float shScale = 4.0 / float(gi.rayCount); // see GiIrradianceBasis
	// the rays and their directions through shared memory, a group's worth at a time
	for (uint rayBase = 0; rayBase < gi.rayCount; rayBase += SHADER_TYPES_GI_GROUP_SIZE)
	{
		uint rayLoad = rayBase + texelIndex;
		if (rayLoad < gi.rayCount)
		{
			gGiSharedRays[texelIndex] = GiUnpackRgba16(gGiRays[rayFirst + rayLoad]);
			gGiSharedDirections[texelIndex] = GiRayDirection(gi, phase, rayLoad);
		}
		GroupMemoryBarrierWithGroupSync();
		uint rayEnd = min(gi.rayCount - rayBase, SHADER_TYPES_GI_GROUP_SIZE);
		for (uint rayIt = 0; rayIt < rayEnd; rayIt++)
		{
			float4 ray = gGiSharedRays[rayIt];
			float3 direction = gGiSharedDirections[rayIt];
			// a ray that met a surface's back counts a fifth of its distance (RTXGI's): the probe is behind that surface
			// that way, and the points on its front, at the same distance, must fail the probe's test. at the full distance
			// a probe behind a thin wall passed for the points on its front (the BVH's rays meet a wall's back exactly where
			// its front is, where the voxels' walk met the wall's back shell a voxel nearer): probes in the space behind
			// salle_de_bain's back wall, by the window's emitter, lit its niches with white patches the surface reference
			// hasn't
			float rayDepth = ray.a < 0.0 ? -ray.a * 0.2 : ray.a;
			for (uint texelIt = 0; texelIt < kTexelsPerThread; texelIt++)
			{
				float cosine = max(dot(texelDirection[texelIt], direction), 0.0);
				float weight = pow(cosine, SHADER_TYPES_GI_DEPTH_SHARPNESS);
				depthSum[texelIt] += weight * float2(rayDepth, rayDepth * rayDepth);
				depthWeight[texelIt] += weight;
				// and the nearest of the rays within about 20 degrees of the texel (see below)
				if (weight > 0.05)
					depthMin[texelIt] = min(depthMin[texelIt], rayDepth);
				// and by a wide cone, for a texel no ray is near (below)
				float wide = cosine * cosine * cosine * cosine;
				wideSum[texelIt] += wide * float2(rayDepth, rayDepth * rayDepth);
				wideWeight[texelIt] += wide;
				if (wide > 0.3)
					wideMin[texelIt] = min(wideMin[texelIt], rayDepth);
			}
			if (texelIndex == 0u)
			{
				GiIrradianceBasis(direction, basis);
				for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
					sh[shIt] += ray.rgb * basis[shIt] * shScale;
				// the rays that met a surface's back, and the nearest such
				if (ray.a < 0.0)
				{
					backfaceCount++;
					if (-ray.a < closestBackface)
					{
						closestBackface = -ray.a; // its real distance: the relocation moves by it
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
		// and the nearest thing the texel's rays met (the second word): the frame's nearest among the rays near the texel,
		// blended as the mean is (a running minimum relaxing by a percent a frame stepped whenever a nearer ray came, and
		// the weights with it: the lion wall's still frames stepped 28 of 255 rather than 14); provisional as the mean
		// is. the mean and its square can't
		// tell a texel toward a wall with a window beside it (half its rays at the wall, half far through the window) from
		// one toward a mid distance, and Chebyshev's test let every point nearer than their mean through: a probe inside
		// Bistro's hotel lit the facade by its windows and its entrance with the interior (see ComputeGiResolve)
		bool near = depthWeight[texelIt] > 1e-4;
		float2 depth = near ? depthSum[texelIt] / depthWeight[texelIt] : (wideWeight[texelIt] > 1e-4 ? float2(-wideSum[texelIt].x, wideSum[texelIt].y) / wideWeight[texelIt] : 0.0);
		float nearest = near ? (depthMin[texelIt] < 1e8 ? depthMin[texelIt] : depth.x) : (wideWeight[texelIt] > 1e-4 ? -(wideMin[texelIt] < 1e8 ? wideMin[texelIt] : -depth.x) : 0.0);
		if (shifted)
		{
			float3 heldTexel = GiUnpackDepth(gGiProbeDepth[gi.depthPreviousFirst + fromIndex * kTexelCount + texel]);
			float2 held = heldTexel.xy;
			float heldNearest = heldTexel.z;
			float alpha = held.x > 0.0 ? (near ? max(1.0 - gi.hysteresis, 1.0 / (age + 1.0)) : 0.0) : (near || held.x == 0.0 ? 1.0 : 0.0);
			depth = lerp(held, depth, alpha);
			float nearestAlpha = heldNearest > 0.0 ? (near ? max(1.0 - gi.hysteresis, 1.0 / (age + 1.0)) : 0.0) : (near || heldNearest == 0.0 ? 1.0 : 0.0);
			nearest = lerp(heldNearest, nearest, nearestAlpha);
		}
		gGiProbeDepth[gi.depthFirst + probeIndex * kTexelCount + texel] = GiPackDepth(depth.x, depth.y, nearest);
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
	// 0.45 of a spacing from the lattice point (its cell's points still interpolate it). the corner's move is taken only
	// where its target is clear of voxels, else the probe stays: it used to go wherever the normals pointed, and a probe
	// behind Bistro's hotel facade, whose two targets were each in the other's voxels (the facade's and the window
	// frames', at 28 cm), jumped between them every frame, one inside the hotel, which its emissive interior lights 30
	// times brighter than the street: the wall by the window lit yellow on alternate frames (the user's heavy flicker).
	// the backface move only while the probe settles (its first two cycles of ray phases, see GiData::rayPhase; again
	// whenever it starts over), then it stays where that left it: each frame's rays are another set, whose nearest back
	// face is elsewhere, so a probe at a quarter of its rays on backs moved every frame within its reach, and the points
	// it lit flickered by up to 100 of 255 (salle_de_bain's niches, whose single sided walls the BVH's rays meet from
	// behind, where the voxels' walk passed them: both faces wrote their voxels)
	float3 offset = GiProbeOffset(history);
	{
		float3 lattice = GiProbeLatticePosition(gi, probe);
		float3 corner = GiCornerAway(gi, lattice + offset);
		if (any(corner != 0.0))
		{
			float3 candidate = corner * (0.45 * gi.probeOrigin.w);
			if (all(GiCornerAway(gi, lattice + candidate) == 0.0))
				offset = candidate;
		}
		else if (age < 2.0 * float(gi.rayPhaseCount) && backfaceCount * 4u > gi.rayCount)
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
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
			sh[shIt] = 0.0;
		for (uint rayIt = 0; rayIt < kRays; rayIt++)
		{
			// a Fibonacci sphere
			float z = 1.0 - (float(rayIt) + 0.5) * 2.0 / float(kRays);
			float radius = sqrt(max(1.0 - z * z, 0.0));
			float phi = float(rayIt) * 2.39996323;
			float3 direction = float3(radius * cos(phi), radius * sin(phi), z);
			GiRayHit hit;
			float4 traced = GiTraceBvh(gi, origin, direction, far, gi.emissionReach * gi.probeOrigin.w, hit)
								? hit.radiance
								: GiMarch(gi, origin, direction, far, false, gi.emissionReach * gi.probeOrigin.w);
			float3 radiance = traced.rgb + traced.a * EnvironmentRadiance(environment, direction, 0.6);
			GiIrradianceBasis(direction, basis);
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
				sh[shIt] += radiance * basis[shIt] * (4.0 / float(kRays));
		}
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
			gGiIrradiance[first + shIt] = float4(sh[shIt], shIt == 2u ? offset.x : (shIt == 3u ? offset.y : (shIt == 4u ? offset.z : 0.0)));
		return;
	}

	// what it held, and its irradiance blended into it
	float3 held[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
		held[shIt] = 0.0;
	if (shifted)
	{
		uint heldFirst = (gi.probePreviousFirst + fromIndex) * SHADER_TYPES_GI_IRRADIANCE_COUNT;
		bool finite = true;
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
		{
			held[shIt] = gGiIrradiance[heldFirst + shIt].rgb;
			finite = finite && !any(isnan(held[shIt])) && !any(isinf(held[shIt]));
		}
		// what isn't finite (a new buffer holds anything) is nothing held
		if (!finite)
		{
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
				held[shIt] = 0.0;
			age = 0.0;
		}
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
				uint aboveFirst = (above.probePreviousFirst + GiProbeIndex(aboveProbe, above.probeSize)) * SHADER_TYPES_GI_IRRADIANCE_COUNT;
				for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
					held[shIt] += weight * gGiIrradiance[aboveFirst + shIt].rgb;
			}
			age = 1.0;
		}
	}
	float alpha = max(1.0 - gi.hysteresis, 1.0 / (age + 1.0));
	// DDGI's adaptive hysteresis, by the probe's own noise: the frame's mean radiance (the faces' mean, luminance) against
	// what it holds, a difference far beyond what its frames usually differ by (four times, and half of what it holds)
	// is a change of the light (one switched or moved, the sun turned), blended in a quarter at a time rather than a
	// thirtieth; the usual difference is averaged over its frames (it grows through a change, so that ends the fast
	// blending on its own). only once it has settled (16 frames): until then the age blends fast anyway
	const float3 kLuminance = float3(0.2126, 0.7152, 0.0722);
	float3 heldMean = 0.0;
	float3 frameMean = 0.0;
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
	{
		heldMean += held[faceIt];
		frameMean += sh[faceIt];
	}
	float heldLuminance = dot(heldMean, kLuminance) / float(SHADER_TYPES_GI_IRRADIANCE_COUNT);
	float difference = abs(dot(frameMean, kLuminance) / float(SHADER_TYPES_GI_IRRADIANCE_COUNT) - heldLuminance);
	bool changed = gi.adapt != 0u && shifted && age >= 16.0 && difference > max(4.0 * noise, 0.5 * heldLuminance);
	if (changed)
		alpha = max(alpha, 0.25);
	noise = shifted ? lerp(noise, difference, 0.05) : difference;

	// what it is blended towards: the mean of its last whole cycle of ray phases (GiData::cycleOffset), not the frame's
	// rays. each frame's set of directions is a 32nd of the cycle's, and a bright source smaller than the sets' spacing
	// (the sky through the gaps between Sponza's curtains, from the corridor behind them) is met by one set of the 32: a
	// probe blending each frame's rays in jumped by it once a cycle and decayed for the other 31 frames, a 32 frame
	// sawtooth of half the light on the floor it lit (the user's pose -9.1511,1.4025,-3.5391,26.261,106.832: pixels from
	// 134 to 84 of 255 and back). a still scene's every cycle is the same directions, so its mean is the same, and the
	// probe settles on it. until its first whole cycle, the running mean of the frames it has; a change of the light (with
	// gi.adapt) starts the cycle again from the frame
	uint cycleFirst = first + gi.cycleOffset * SHADER_TYPES_GI_IRRADIANCE_COUNT;
	uint meanFirst = cycleFirst + gi.cycleOffset * SHADER_TYPES_GI_IRRADIANCE_COUNT;
	float3 cycleSum[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	float3 cycleMean[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	float cycleFrames = 0.0;
	bool hasMean = false;
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
	{
		cycleSum[shIt] = 0.0;
		cycleMean[shIt] = 0.0;
	}
	if (shifted && age > 0.0 && !changed)
	{
		uint heldCycleFirst = (gi.probePreviousFirst + fromIndex + gi.cycleOffset) * SHADER_TYPES_GI_IRRADIANCE_COUNT;
		uint heldMeanFirst = heldCycleFirst + gi.cycleOffset * SHADER_TYPES_GI_IRRADIANCE_COUNT;
		cycleFrames = gGiIrradiance[heldCycleFirst].w;
		hasMean = gGiIrradiance[heldMeanFirst].w > 0.0;
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
		{
			cycleSum[shIt] = gGiIrradiance[heldCycleFirst + shIt].rgb;
			cycleMean[shIt] = gGiIrradiance[heldMeanFirst + shIt].rgb;
		}
	}
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
		cycleSum[shIt] += sh[shIt];
	cycleFrames += 1.0;
	float3 target[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	bool cycleDone = cycleFrames >= float(gi.rayPhaseCount);
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
	{
		if (cycleDone)
			cycleMean[shIt] = cycleSum[shIt] / cycleFrames;
		target[shIt] = changed ? sh[shIt] : ((hasMean || cycleDone) ? cycleMean[shIt] : cycleSum[shIt] / cycleFrames);
	}
	hasMean = hasMean || cycleDone;
	if (cycleDone)
	{
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
			cycleSum[shIt] = 0.0;
		cycleFrames = 0.0;
	}
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
	{
		gGiIrradiance[cycleFirst + shIt] = float4(cycleSum[shIt], shIt == 0u ? cycleFrames : 0.0);
		gGiIrradiance[meanFirst + shIt] = float4(cycleMean[shIt], shIt == 0u && hasMean ? 1.0 : 0.0);
	}
	// and whether it is inside something still (more than a quarter of its rays met a surface's back: face 5's w,
	// DDGI's classification): the resolve skips it (Sponza's outer walls are thicker than a probe may move, and a probe
	// between their shells, which sees 0.3 of a spacing all around, passed as seeing the points of the corridor beside)
	// the share of its rays that met a surface's back, blended over its frames as its light is: inside while above a
	// quarter (kGiInsideShare). this frame's share alone swings with the frame's ray set, and a probe near a quarter was
	// skipped by the resolve on alternate frames, its light and its neighbours' taking turns at the points around it
	// (salle_de_bain's niches flickered by up to 100 of 255)
	float frameInside = float(backfaceCount) / float(gi.rayCount);
	float inside = shifted ? lerp(gGiIrradiance[(gi.probePreviousFirst + fromIndex) * SHADER_TYPES_GI_IRRADIANCE_COUNT + 5].w, frameInside, alpha) : frameInside;
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
	{
		float kept = shIt == 0u ? min(age + 1.0, 65535.0) : (shIt == 1u ? noise : (shIt == 2u ? offset.x : (shIt == 3u ? offset.y : (shIt == 4u ? offset.z : (shIt == 5u ? inside : 0.0)))));
		gGiIrradiance[first + shIt] = float4(lerp(held[shIt], target[shIt], alpha), kept);
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
		// and the light it holds: the irradiance over pi along +-x, +-y, +-z (its faces), and the frame's rays' mean
		for (uint axisIt = 0; axisIt < 6; axisIt++)
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + 36 + axisIt] = asuint(dot(max(gGiIrradiance[first + axisIt].rgb, 0.0), kLuminance));
		gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + 42] = asuint(dot(frameMean, kLuminance) / float(SHADER_TYPES_GI_IRRADIANCE_COUNT));
		// and the volumes at its voxel, as the last frame's resolve left them (volume 0: red along +x, -x, +y, -y; volume
		// 4: blue along +z and -z, and the weight in w)
		{
			int3 at = int3(floor(GiVoxelCoordinate(gi, position, 0)));
			uint3 texel = uint3(clamp(at, int3(0), int3(GiLevelSize(gi, 0)) - 1));
			texel.z += gi.volumeImage.w;
			float4 v0 = gGiVolumeImages[0][texel];
			float4 v6 = gGiVolumeImages[4][texel];
			for (uint valueIt = 0; valueIt < 4; valueIt++)
			{
				gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + 43 + valueIt] = asuint(v0[valueIt]);
				gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + 47 + valueIt] = asuint(v6[valueIt]);
			}
		}
		// and along -z from the point asked for, 16 voxels: whether occupied, and the volumes' red along +x and weight
		for (uint stepIt = 0; stepIt < 16; stepIt++)
		{
			float3 p = gi.dumpPoint.xyz - float3(0.0, 0.0, float(stepIt) * gi.volumeOrigin.w);
			int3 at = int3(floor(GiVoxelCoordinate(gi, p, 0)));
			bool in = GiInside(at, GiLevelSize(gi, 0));
			uint3 texel = uint3(clamp(at, int3(0), int3(GiLevelSize(gi, 0)) - 1));
			uint occupied = in ? (GiVoxelAt(gi, texel).albedo >> 24) : 0u;
			texel.z += gi.volumeImage.w;
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + 51 + stepIt * 3] = asuint(float(occupied));
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + 52 + stepIt * 3] = asuint(gGiVolumeImages[0][texel].x);
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + 53 + stepIt * 3] = asuint(gGiVolumeImages[4][texel].w);
		}
		// and after the depth map, the first 16 rays' directions' y and depths
		for (uint rayIt = 0; rayIt < 16u && rayIt < gi.rayCount; rayIt++)
		{
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + rayIt * 2] = asuint(GiRayDirection(gi, phase, rayIt).y);
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + kTexelCount + rayIt * 2 + 1] = asuint(GiUnpackRgba16(gGiRays[rayFirst + rayIt]).a);
		}
		for (uint texelIt = 0; texelIt < kTexelCount; texelIt++)
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 16 + texelIt] = asuint(GiUnpackDepth(gGiProbeDepth[gi.depthFirst + probeIndex * kTexelCount + texelIt]).x);
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
		return GiRadianceAt(gi, at).rgb;
	}
	return float3(1.0, 0.0, 1.0);
}

// the finest clip that holds the point well inside (more than a probe spacing from its edges, where its probes are
// clamped), blended into the next over the 8 voxels after that, so the clips meet without a seam
// the surface reference (DEBUG_VIEW_GI_SURFACE_REFERENCE): the irradiance at the point itself over pi, 256 cosine weighted
// rays from it (off it two voxels along its normal, past the voxels of its back face in front of it) through the finest
// clip's voxels that holds it and the coarser ones', the environment past them
// the surface reference's ray origin in a clip: off the point two voxels along its normal, and out of the voxels of an
// adjacent surface at a concave edge (its shell is two voxels thick, see ComputeGiVoxelize) along the voxel's normal
// turned toward the eye (view: the point is seen, so that side is open; toward the point where there is none), a voxel
// and a half at a time, else every ray starts in their radiance (CornellBox's red wall drew a line along the floor).
// along the normal as stored, the inner shell voxel's (which points into the solid it stands for) led further in, and
// toward the point, a pixel on a rib's side within half a voxel of the vault went either way: the vault's ribs drew
// black lines
float3 GiReferenceOrigin(GiData gi, float3 position, float3 normal, float3 view)
{
	float3 origin = position + normal * (2.0 * gi.volumeOrigin.w);
	for (uint stepIt = 0; stepIt < 3; stepIt++)
	{
		int3 at = int3(floor(GiVoxelCoordinate(gi, origin, 0)));
		if (!GiInside(at, GiLevelSize(gi, 0)))
			break;
		GiVoxel voxel = GiVoxelAt(gi, uint3(at));
		if ((voxel.albedo >> 24) == 0u)
			break;
		float3 out = GiVoxelNormal(voxel.normal);
		float3 center = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
		if (dot(out, any(view != 0.0) ? view : position - center) < 0.0)
			out = -out;
		origin += out * (1.5 * gi.volumeOrigin.w);
	}
	return origin;
}

// the surface reference (DEBUG_VIEW_GI_SURFACE_REFERENCE): the irradiance at the point itself over pi, 256 cosine weighted
// rays from it (see GiReferenceOrigin) through the clips' voxels, the environment past them. the clips blended as the
// shading blends them (GiSampleAt: each by its weight of what the finer ones leave): each ray is one clip's, the rays
// shared out by the clips' weights, so a pixel costs the same however many clips hold it (tracing all of the rays in
// each clip ran past Metal's command queue timeout); from the finest clip alone, the edge of its box around the eye
// cut the vault in two shades, the finer shells on one side and the coarser on the other
float3 GiPixelReference(float3 position, float3 normal, float3 view)
{
	uint clipCount = gGi[gPushConstants.frameIndex][0].clipCount;
	GiData last = GiClip(clipCount - 1);
	float far = length(float3(last.volumeSize.xyz)) * last.volumeOrigin.w;
	float3 tangent = normalize(abs(normal.y) < 0.9 ? cross(normal, float3(0, 1, 0)) : cross(normal, float3(1, 0, 0)));
	float3 bitangent = cross(normal, tangent);
	EnvironmentData environment = gEnvironment[0];
	uint kRays = gGi[gPushConstants.frameIndex][0].referenceRayCount;
	// the spiral turned and shifted by a hash of the point: the same directions everywhere made radial streaks (each
	// ray's hit or miss the same over a whole surface); turned per point, they are noise instead
	uint hash = asuint(position.x * 7919.0) * 73856093u ^ asuint(position.y * 7919.0) * 19349663u ^ asuint(position.z * 7919.0) * 83492791u;
	hash = (hash ^ (hash >> 16)) * 0x45d9f3bu;
	float turn = float(hash & 0xFFFFu) / 65536.0 * 6.28318531;
	float shift = float(hash >> 16) / 65536.0;
	float3 sum = 0.0;
	float remaining = 1.0;
	uint rayFirst = 0;
	for (uint clipIt = 0; clipIt < clipCount && rayFirst < kRays; clipIt++)
	{
		float weight = clipIt + 1 == clipCount ? 1.0 : GiClipWeight(GiClipViewAt(clipIt), position);
		if (weight <= 0.0)
			continue;
		uint rayEnd = clipIt + 1 == clipCount ? kRays : min(rayFirst + uint(round(remaining * weight * float(kRays))), kRays);
		remaining *= 1.0 - weight;
		if (rayEnd <= rayFirst)
			continue;
		GiData gi = GiClip(clipIt);
		float3 origin = GiReferenceOrigin(gi, position, normal, view);
		for (uint rayIt = rayFirst; rayIt < rayEnd; rayIt++)
		{
			// cosine weighted, a Fibonacci spiral over the disk
			float u = frac((float(rayIt) + shift) / float(kRays));
			float r = sqrt(u);
			float phi = float(rayIt) * 2.39996323 + turn;
			float3 direction = normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + normal * sqrt(max(1.0 - u, 0.0)));
			GiRayHit hit;
			float4 traced = GiTraceBvh(gi, origin, direction, far, 0.0, hit, false) ? hit.radiance : GiMarch(gi, origin, direction, far, false);
			sum += traced.rgb + traced.a * EnvironmentRadiance(environment, direction, 0.6);
		}
		rayFirst = rayEnd;
	}
	return sum / float(kRays);
}

float3 GiIrradiance(float3 position, float3 normal, uint debug, float3 view = 0.0)
{
	if (debug == 13u)
		return GiPixelReference(position, normal, view);
	if (debug == 7u || debug == 8u)
		return GiClipDebug(position, normal, debug == 8u);
	if (debug == 12u)
		return GiProbesDebug(position, normal);
	if (debug == 14u)
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

// per voxel of a clip, after its probes: the irradiance faces of the 8 probes around its center, each weighed by how
// much of it the voxel sees (trilinear by its center; whether the probe sees the voxel's point, by the probe's depth map:
// Chebyshev's inequality on the mean and variance of the distance it sees that way, as DDGI tests it; and how much the
// probe faces the voxel's open side), and the sum of the weights. a point interpolating the voxels around it so sees each
// probe as much as the voxels around it do, at 4 filtered reads. a group per block of 4x4x4 voxels (a flag's block, see
// ComputeGiFlags: nothing near it, the block is skipped), whose 8 probes (the block spans one cell, the spacing being a
// multiple of 8) are read into shared memory once rather than 8 a voxel (1.2 KB each)
// the surface cache (gGiCache, GiData::cacheRayCount): per voxel near a surface, the irradiance along the axes (an
// ambient cube, as a probe's) that rays from the voxel itself saw, a few a frame, accumulated over frames (the resolve,
// GiCacheUpdate), then filtered with its neighbours' on the same surface and blended into the voxel's light by the
// samples behind it (ComputeGiCacheFilter). the probes are 8 voxels apart in their clip, 1.8 m in Sponza's last, and
// spread the light through a gap of a few tens of centimetres (the sky under its curtains) into spots a probe cell wide,
// which changed as the finer clips took the floor over; a voxel's own rays resolve it at the voxel's size, noisily (a
// small bright source is met by few of its rays), which the filter takes out where it is noise. 3 uint4 an entry: the 6
// faces' rgb as half floats (9 words); the samples its mean is of (at most GiData::cacheSampleCount) and the rays traced
// (its directions' sequence), 16 bits each; the mean of its samples' squares (GiCacheSample: the light along the voxel's
// open side, whose mean the faces hold), which with the mean is their variance; and the voxel's open side (octahedral, 8
// bits a coordinate) and the rays that started inside something (in sixteenths), 16 bits each
struct GiCacheEntry
{
	float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	uint count;
	uint inside;
	uint sequence;
	float moment;
	float3 normal;
};

// how many samples the probes' light counts as, against a voxel's: its light is the cache's by its samples over them
// and this
static const float kGiCachePrior = 16.0;
// how many samples of their mean square a voxel's variance is taken with besides its own (a voxel of few samples is
// noise, however alike they came out: one sample has no variance)
static const float kGiCacheVariancePrior = 4.0;
// the most samples the noise the filter stops at (its edges, see ComputeGiCacheFilter) is taken as the mean of: the variance
// of a mean of the voxel's samples, as it was, shrank as they grew to 1024, the filter smoothed less and less, and the light
// it had spread from bright voxels into darker ones withdrew for as long (Sponza's interior still darkened 2400 frames
// after a cold start, its probes settled by 300); held at this, the filter is the same once a voxel has as many
static const float kGiCacheFilterCount = 64.0;
// how many samples a voxel new to its clip takes the coarser clip's entry as (see GiCacheUpdate)
static const uint kGiCacheSeedCount = 8u;
// how often a voxel no lookup reads traces a ray (see GiCacheUpdate), and how many frames after a lookup read its block
// it still counts as read (see ComputeGiResolve)
static const uint kGiCacheIdleFrameCount = 8u;
static const uint kGiCacheSeenFrameCount = 2u;
static const float3 kGiCacheLuminance = float3(0.2126, 0.7152, 0.0722);

GiCacheEntry GiCacheEmpty()
{
	GiCacheEntry entry;
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		entry.faces[faceIt] = 0.0;
	entry.count = 0u;
	entry.inside = 0u;
	entry.sequence = 0u;
	entry.moment = 0.0;
	entry.normal = 0.0;
	return entry;
}

// a unit vector in 16 bits: octahedral, 8 bits a coordinate (the filter compares them, see ComputeGiCacheFilter)
uint GiCachePackNormal(float3 n)
{
	n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
	float2 p = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
	uint2 q = uint2(round(saturate(p * 0.5 + 0.5) * 255.0));
	return q.x | (q.y << 8);
}

float3 GiCacheUnpackNormal(uint packed)
{
	float2 p = float2(packed & 0xFFu, (packed >> 8) & 0xFFu) / 255.0 * 2.0 - 1.0;
	float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
	if (n.z < 0.0)
		n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
	return normalize(n);
}

// the faces of an entry or a filtered record (see GiCacheTap): their rgb as half floats, in the first 9 words
void GiCacheUnpackFaces(uint4 words[3], out float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT])
{
	float halves[2 * 9];
	for (uint wordIt = 0; wordIt < 9; wordIt++)
	{
		float2 pair = GiUnpackHalf2(words[wordIt / 4u][wordIt % 4u]);
		halves[wordIt * 2u] = pair.x;
		halves[wordIt * 2u + 1u] = pair.y;
	}
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		faces[faceIt] = float3(halves[faceIt * 3u], halves[faceIt * 3u + 1u], halves[faceIt * 3u + 2u]);
}

void GiCachePackFaces(float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT], out uint packed[9])
{
	float halves[2 * 9];
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
	{
		halves[faceIt * 3u] = faces[faceIt].r;
		halves[faceIt * 3u + 1u] = faces[faceIt].g;
		halves[faceIt * 3u + 2u] = faces[faceIt].b;
	}
	for (uint wordIt = 0; wordIt < 9; wordIt++)
		packed[wordIt] = GiPackHalf2(float2(halves[wordIt * 2u], halves[wordIt * 2u + 1u]));
}

GiCacheEntry GiCacheLoad(uint index)
{
	uint4 words[3] = {gGiCache[index * 3u], gGiCache[index * 3u + 1u], gGiCache[index * 3u + 2u]};
	GiCacheEntry entry;
	GiCacheUnpackFaces(words, entry.faces);
	entry.count = words[2].y & 0xFFFFu;
	entry.sequence = words[2].y >> 16;
	entry.moment = asfloat(words[2].z);
	entry.inside = words[2].w >> 16;
	entry.normal = entry.count != 0u ? GiCacheUnpackNormal(words[2].w & 0xFFFFu) : 0.0;
	return entry;
}

void GiCacheStore(uint index, GiCacheEntry entry)
{
	uint packed[9];
	GiCachePackFaces(entry.faces, packed);
	gGiCache[index * 3u] = uint4(packed[0], packed[1], packed[2], packed[3]);
	gGiCache[index * 3u + 1u] = uint4(packed[4], packed[5], packed[6], packed[7]);
	gGiCache[index * 3u + 2u] = uint4(packed[8], min(entry.count, 0xFFFFu) | ((entry.sequence & 0xFFFFu) << 16), asuint(entry.moment),
									  GiCachePackNormal(entry.normal) | (min(entry.inside, 0xFFFFu) << 16));
}

void GiCacheClear(uint index)
{
	gGiCache[index * 3u] = 0u;
	gGiCache[index * 3u + 1u] = 0u;
	gGiCache[index * 3u + 2u] = 0u;
}

// the cache's storage: a page per block of 4x4x4 voxels that holds entries and was read lately, its 64 entries in the
// order of a group's threads, from a pool of GiData::cachePageCapacity (gGiCache). the clip's page tables
// (gGiCacheBlocks: this frame's and the last frame's, swapped each frame, so that a clip that scrolled finds its blocks'
// pages where they were) hold two words a block: its page + 1 (0: none), with kGiCacheWanted where a voxel of it held an
// entry (the resolve sets it), and the last frame a lookup read it (or around it, see ComputeGiResolve). a block keeps
// its page while it is near a surface, wanted it and was read within GiData::cacheKeepFrameCount frames (GiCacheKeeps),
// and is updated in place; the release (ComputeGiCacheRelease) frees the others, and the resolve takes pages for the
// blocks that wanted one last frame and are read now. it was an entry per voxel of every clip, two halves and the
// filter's scratch, 48 bytes each: 6.4 GB at resolution 384, where Sponza then took 14.9 GB of a 16 GB gpu; paged for
// every block near a surface (within the flags' reach), 1.1 GB, though half of those blocks' voxels held no entry and a
// tenth of them were read in a frame
static const uint kGiCacheWanted = 0x80000000u;

// a block's first word in a page table (GiData::cacheTableFirst or cacheTablePreviousFirst)
uint GiCacheTableWord(uint first, uint blockIndex)
{
	return first + 2u * blockIndex;
}

// the page + 1 of a page table's word
uint GiCachePageOf(uint word)
{
	return word & ~kGiCacheWanted;
}

// whether a block's page (its words in the last frame's table) is carried over: it is near a surface now, wanted a page,
// and was read within GiData::cacheKeepFrameCount frames. the release frees the pages of the others, by the same test
bool GiCacheKeeps(GiData gi, uint word, uint lastRead, bool nearSurface)
{
	return GiCachePageOf(word) != 0u && (word & kGiCacheWanted) != 0u && nearSurface && gi.frameStamp - lastRead <= gi.cacheKeepFrameCount;
}

uint GiCacheVoxelInBlock(uint3 at)
{
	return (at.x & 3u) | ((at.y & 3u) << 2) | ((at.z & 3u) << 4);
}

uint GiCacheBlockIndex(uint3 at, uint3 blocks)
{
	uint3 block = at >> 2;
	return block.x + blocks.x * (block.y + blocks.y * block.z);
}

// a voxel's entry in its block's page (the page + 1, as the tables hold it)
uint GiCacheEntryIndex(uint page, uint3 at)
{
	return (page - 1u) * 64u + GiCacheVoxelInBlock(at);
}

// a voxel's page this frame, by its clip's table (0: its block has none)
uint GiCachePageAt(GiData gi, uint3 at, uint3 blocks)
{
	return GiCachePageOf(gGiCacheBlocks[GiCacheTableWord(gi.cacheTableFirst, GiCacheBlockIndex(at, blocks))]);
}

// the free pages (gGiCacheFree: their count, the records taken this frame, then the pages), pushed by the release and
// by the pool's growth (ComputeGiCachePrepare) and popped by the resolve, never both in one dispatch: a pop that finds
// none gives back what it took. a page + 1, 0 when none is free (the block keeps no cache this frame, and the pool grows
// by what the resolve counted, GI_STATISTIC_CACHE_BLOCKS)
uint GiCachePopPage()
{
	uint count;
	InterlockedAdd(gGiCacheFree[0], 0xFFFFFFFFu, count);
	if (int(count) <= 0)
	{
		InterlockedAdd(gGiCacheFree[0], 1u);
		return 0u;
	}
	return gGiCacheFree[2u + count - 1u] + 1u;
}

void GiCachePushPage(uint page)
{
	uint count;
	InterlockedAdd(gGiCacheFree[0], 1u, count);
	gGiCacheFree[2u + count] = page;
}

// the filter's records of a block a lookup read (see ComputeGiCacheFilter), this frame's only: a record + 1, 0 when the
// GiData::cacheRecordCapacity are taken (the block isn't filtered, and the records grow by GI_STATISTIC_CACHE_ACTIVE_BLOCKS)
uint GiCacheTakeRecord(GiData gi)
{
	uint taken;
	InterlockedAdd(gGiCacheFree[1], 1u, taken);
	return taken < gi.cacheRecordCapacity ? taken + 1u : 0u;
}

// a voxel's record in one of gGiCacheRecords' halves (the passes write them in turn)
uint GiCacheRecordIndex(GiData gi, uint half, uint record, uint3 at)
{
	return (half * gi.cacheRecordCapacity + record - 1u) * 64u + GiCacheVoxelInBlock(at);
}

// whether an entry is light to filter: samples, and not a voxel inside something (above a quarter of its rays started
// in something, as a probe's, kGiInsideShare)
// the inside rays (see GiCacheEntry) are kept in sixteenths, so that their scaling with the samples (see GiCacheUpdate)
// doesn't round away
static const uint kGiCacheInsideScale = 16u;

bool GiCacheValid(GiCacheEntry entry)
{
	float inside = float(entry.inside) / float(kGiCacheInsideScale);
	return entry.count != 0u && inside <= kGiInsideShare * (float(entry.count) + inside);
}

// a ray's sample of the light along a voxel's open side (normal), in luminance: what it adds to the faces the normal's
// components point at, each by the component squared, as GiEvaluate reads them. their mean is the faces' light along it
// (GiCacheLight)
float GiCacheSample(float luminance, float3 direction, float3 normal)
{
	float3 along = max(direction * select(normal >= 0.0, 1.0, -1.0), 0.0);
	return 4.0 * luminance * dot(normal * normal, along);
}

// the faces' light along a direction (a voxel's open side), in luminance (see GiEvaluate)
float GiCacheLight(float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT], float3 normal)
{
	float3 w = normal * normal;
	float3 x = normal.x >= 0.0 ? faces[0] : faces[1];
	float3 y = normal.y >= 0.0 ? faces[2] : faces[3];
	float3 z = normal.z >= 0.0 ? faces[4] : faces[5];
	return dot(max(w.x * x + w.y * y + w.z * z, 0.0), kGiCacheLuminance);
}

// the share of a voxel's cache rays over the hemisphere its open side faces (see GiCacheDirection)
static const float kGiCacheForwardShare = 0.75;

// a voxel's sample of its sequence: a direction of the sphere by Roberts' R2 sequence (even over the sphere at any count,
// so its mean converges as a quadrature's rather than a random sum's), offset per voxel by a hash of its place in the
// world (the same wherever its clip has scrolled to), so that the voxels around a point miss differently. about a voxel's
// open side (normal: unit, or zero for the sphere evenly), kGiCacheForwardShare of them over the hemisphere it faces, whose
// faces are what the shading reads mostly, and the rest over the other (the first coordinate stratified by the side, so
// the sequence stays even), each sample weighed by its side's density (GiCacheEstimate). over the open side's hemisphere
// alone, Sponza's interior came out 6% darker: the lookups read faces of the other side too, at edges and corners
float3 GiCacheDirection(int3 world, uint sequence, float3 normal)
{
	uint hash = uint(world.x) * 73856093u ^ uint(world.y) * 19349663u ^ uint(world.z) * 83492791u;
	hash ^= hash >> 16;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;
	float2 offset = float2(hash & 0xFFFFu, hash >> 16) / 65536.0;
	const float kG = 1.32471795724474602596;
	float2 uv = frac(offset + float(sequence) * float2(1.0 / kG, 1.0 / (kG * kG)));
	bool sided = dot(normal, normal) > 0.5;
	float z = !sided ? 1.0 - 2.0 * uv.x
			  : uv.x < kGiCacheForwardShare ? uv.x / kGiCacheForwardShare
										: -(uv.x - kGiCacheForwardShare) / (1.0 - kGiCacheForwardShare);
	float radius = sqrt(max(1.0 - z * z, 0.0));
	float phi = 6.28318530718 * uv.y;
	float3 local = float3(radius * cos(phi), radius * sin(phi), z);
	if (!sided)
		return local;
	float3 t;
	float3 b;
	ShadowBasis(normal, t, b);
	return local.x * t + local.y * b + local.z * normal;
}

// what a cache ray's sample stands for, as a uniform sphere's sample would (1: the sphere evenly): its side's
// hemisphere's density against the sphere's (see GiCacheDirection)
float GiCacheEstimate(float3 direction, float3 normal)
{
	if (dot(normal, normal) <= 0.5)
		return 1.0;
	return dot(direction, normal) >= 0.0 ? 0.5 / kGiCacheForwardShare : 0.5 / (1.0 - kGiCacheForwardShare);
}

// where a surface's voxel's cache rays start (see GiCacheUpdate): out from its center along its open side (away) by a
// voxel, as the resolve takes the probes' light (see ComputeGiResolve), but only halfway to a surface facing back at it
// (by how far it is open along its normal, see GiOpenAlong: a ray of its own each update cost Sponza 0.3 ms a frame).
// between two surfaces a voxel or two apart (CornellBox's sphere a voxel off its wall), the voxel out along the wall's
// open side was in the sphere: the rays started inside it, the entries were inside (GiCacheValid) and held no light of
// their own, the probes around were all hidden, and the wall drew a black crescent along the sphere, stepped at the
// voxels
float3 GiCacheOrigin(GiData gi, uint3 at, float3 away)
{
	uint3 size = GiLevelSize(gi, 0);
	float open = GiUnpackRgba16(gGiRadiance[gi.directFirst + GiVoxelIndex(at, size)]).a * gi.volumeOrigin.w;
	float3 center = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	return center + away * min(gi.volumeOrigin.w, 0.5 * open);
}

// a voxel's entry carried over from the last frame (in place, in its block's page: page, the page + 1; none in a page
// new to the block, fresh, nor after a reset or a change of the lights) and cacheRayCount more rays from center (the
// voxel's, as the resolve moved it; a surface's voxel's, surface, from out along its open side, normal: see
// GiCacheOrigin) through the BVHs, or the voxels, as the probes' rays are, each blended into the faces' mean and its
// square's (GiCacheSample) by the reciprocal of its samples, at most cacheSampleCount (32 in a block something moving
// went through this frame). a ray that meets a surface's back within a voxel and a half started inside something: not
// a sample
void GiCacheUpdate(GiData gi, uint3 at, uint3 size, uint blockIndex, uint page, bool fresh, float3 center, float3 normal, bool surface, bool active)
{
	uint index = GiCacheEntryIndex(page, at);
	GiCacheEntry entry = GiCacheEmpty();
	if (gi.reset == 0u && gi.cacheReset == 0u)
	{
		if (!fresh)
			entry = GiCacheLoad(index);
		else if (gi.clip + 1u < gi.clipCount)
		{
			// a voxel new to the clip (it scrolled in, or its block came near a surface) starts from the coarser clip's
			// entry where it is, as kGiCacheSeedCount samples, which its own rays soon outweigh: from nothing, it took the
			// probes' light until it had samples, and the light along Sponza's curtains faded in and out near the eye as
			// the finer clips scrolled. the coarser block is read where its page is carried over (GiCacheKeeps), so that
			// it is its own still, not freed and taken by another: by its last frame's table, at its place then
			GiData coarser = GiClip(gi.clip + 1u);
			uint3 coarserSize = GiLevelSize(coarser, 0);
			uint3 coarserBlocks = GiLevelSize(coarser, SHADER_TYPES_GI_FLAG_LEVEL);
			int3 voxel = int3(floor(GiVoxelCoordinate(coarser, center, 0)));
			int3 seed = voxel + coarser.scroll.xyz;
			if (GiInside(voxel, coarserSize) && GiInside(seed, coarserSize))
			{
				uint word = GiCacheTableWord(coarser.cacheTablePreviousFirst, GiCacheBlockIndex(uint3(seed), coarserBlocks));
				bool nearSurface = (gGiFlags[GiFlagIndex(coarser, GiCacheBlockIndex(uint3(voxel), coarserBlocks))] & 1u) != 0u;
				uint seedPage = GiCacheKeeps(coarser, gGiCacheBlocks[word], gGiCacheBlocks[word + 1u], nearSurface) ? GiCachePageOf(gGiCacheBlocks[word]) : 0u;
				GiCacheEntry seeded = seedPage != 0u ? GiCacheLoad(GiCacheEntryIndex(seedPage, uint3(seed))) : GiCacheEmpty();
				if (GiCacheValid(seeded))
				{
					entry = seeded;
					entry.count = min(seeded.count, kGiCacheSeedCount);
					entry.inside = 0u;
					entry.sequence = 0u;
				}
			}
		}
	}
	bool finite = !isnan(entry.moment) && !isinf(entry.moment);
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		finite = finite && !any(isnan(entry.faces[faceIt])) && !any(isinf(entry.faces[faceIt]));
	if (!finite)
		entry = GiCacheEmpty();
	// the open side as stored, so that the samples are of the light the filter reads along it
	entry.normal = GiCacheUnpackNormal(GiCachePackNormal(normal));
	bool moving = gi.moving != 0u && gGiFlags[GiFlagIndex(gi, blockIndex) + 1u] == gi.frameStamp;
	uint cap = min(moving ? min(gi.cacheSampleCount, 32u) : gi.cacheSampleCount, 0xFFFFu);
	entry.count = min(entry.count, cap);
	int3 world = int3(round(gi.volumeOrigin.xyz / gi.volumeOrigin.w)) + int3(at);
	// a voxel no lookup reads (its block not active, see ComputeGiResolve) traces a ray every kGiCacheIdleFrameCount
	// frames (staggered by its place), so that it isn't cold when one does
	uint rayCount = gi.cacheRayCount;
	if (!active)
	{
		uint hash = uint(world.x) * 73856093u ^ uint(world.y) * 19349663u ^ uint(world.z) * 83492791u;
		rayCount = (hash + gi.frameStamp) % kGiCacheIdleFrameCount == 0u ? 1u : 0u;
	}
	// and once settled (GiData::cacheSettled), where nothing moves, every other frame (staggered by its place): its mean is
	// of the same samples, over twice the frames, of a light that doesn't change
	else if (gi.cacheSettled != 0u && !moving)
	{
		uint hash = uint(world.x) * 73856093u ^ uint(world.y) * 19349663u ^ uint(world.z) * 83492791u;
		rayCount = ((hash + gi.frameStamp) & 1u) == 0u ? rayCount : 0u;
	}
	// the dumped point's voxel's entry (SPEEDO_GI_DUMP, see GI_DUMP_CACHE_FIRST): its counts, its rays' origin and the clip
	// they walk, its voxel, the light it holds per face, and the frame's first 8 rays (direction, distance or -1 where it
	// met nothing, light, whether it met a surface's back): what found the slot behind salle_de_bain's bench lit (see
	// GiOpenAlong)
	bool dumped = gi.dumpProbe != ~0u && all(int3(at) == int3(floor(GiVoxelCoordinate(gi, gi.dumpPoint.xyz, 0))));
	uint dumpFirst = GI_STATISTIC_COUNT + gi.clip * GI_DUMP_FLOAT_COUNT + GI_DUMP_CACHE_FIRST;
	if (rayCount != 0u)
	{
		float3 origin = surface ? GiCacheOrigin(gi, at, normal) : center;
		GiData last = GiClip(gi.clipCount - 1);
		float far = length(float3(last.volumeSize.xyz)) * last.volumeOrigin.w;
		GiData walk = GiClip(GiFinestClipHolding(origin, gi.clip));
		float insideReach = 1.5 * gi.volumeOrigin.w;
		if (dumped)
		{
			float values[3] = {origin.x, origin.y, origin.z};
			for (uint valueIt = 0; valueIt < 3; valueIt++)
				gGiStatistics[gPushConstants.frameIndex][dumpFirst + 2 + valueIt] = asuint(values[valueIt]);
			gGiStatistics[gPushConstants.frameIndex][dumpFirst + 21] = asuint(float(walk.clip));
		}
		for (uint rayIt = 0; rayIt < rayCount; rayIt++)
		{
			float3 direction = GiCacheDirection(world, entry.sequence++, normal);
			// the rays mostly over the open side's hemisphere where it has one (see GiCacheDirection), each sample weighed by
			// its side's density: half the rays over the sphere added little to the faces the shading reads, and their mean
			// was as noisy as of half as many
			float estimate = GiCacheEstimate(direction, normal);
			float4 traced;
			GiRayHit hit;
			bool bvh = GiTraceBvh(walk, origin, direction, far, gi.volumeOrigin.w, hit);
			if (dumped && rayIt < 8u)
			{
				float3 shown = bvh ? hit.radiance.rgb + hit.radiance.a * EnvironmentRadiance(gEnvironment[0], direction, 0.6) : 0.0;
				float values[6] = {direction.x, direction.y, direction.z, bvh && hit.radiance.a == 0.0 ? hit.distance : -1.0, dot(shown, kGiCacheLuminance), bvh && hit.backface ? 1.0 : 0.0};
				for (uint valueIt = 0; valueIt < 6; valueIt++)
					gGiStatistics[gPushConstants.frameIndex][dumpFirst + 30 + rayIt * 6 + valueIt] = asuint(values[valueIt]);
			}
			if (bvh)
			{
				if (hit.backface && hit.distance < insideReach)
				{
					entry.inside += uint(round(float(kGiCacheInsideScale) * estimate));
					continue;
				}
				traced = hit.radiance;
			}
			else
				traced = GiMarch(walk, origin, direction, far, true, gi.volumeOrigin.w);
			float3 radiance = traced.rgb + traced.a * EnvironmentRadiance(gEnvironment[0], direction, 0.6);
			if (any(isnan(radiance)) || any(isinf(radiance)))
				continue;
			float basis[SHADER_TYPES_GI_IRRADIANCE_COUNT];
			GiIrradianceBasis(direction, basis);
			entry.count = min(entry.count + 1u, cap);
			float weight = 1.0 / float(entry.count);
			for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
				entry.faces[faceIt] += (radiance * basis[faceIt] * (4.0 * estimate) - entry.faces[faceIt]) * weight;
			float light = GiCacheSample(dot(radiance, kGiCacheLuminance), direction, entry.normal) * estimate;
			entry.moment += (light * light - entry.moment) * weight;
		}
	}
	// the samples and the inside rays over one window, the cap's: past it both scaled down together, so that the inside
	// rays' share stays the share of the latest rays. clamped apart, the samples stopped at the cap while the inside rays
	// went on to it, so a voxel with a tenth of its rays inside crossed a quarter (no light) after 3400 frames, a fifth
	// after 1700, and Sponza's interior darkened for thousands of frames after a cold start
	float inside = float(entry.inside) / float(kGiCacheInsideScale);
	float total = float(entry.count) + inside;
	if (total > float(cap))
	{
		float scale = float(cap) / total;
		entry.count = uint(float(entry.count) * scale);
		entry.inside = uint(round(float(entry.inside) * scale));
	}
	GiCacheStore(index, entry);
	if (dumped)
	{
		float3 voxelCenter = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
		GiVoxel voxel = gGiVoxels[gi.voxelFirst + GiVoxelIndex(at, size)];
		float3 voxelNormal = GiVoxelNormal(voxel.normal);
		float values[30] = {
			float(entry.count), float(entry.inside) / float(kGiCacheInsideScale), 0.0, 0.0, 0.0,
			voxelCenter.x, voxelCenter.y, voxelCenter.z, normal.x, normal.y, normal.z,
			dot(entry.faces[0], kGiCacheLuminance), dot(entry.faces[1], kGiCacheLuminance), dot(entry.faces[2], kGiCacheLuminance),
			dot(entry.faces[3], kGiCacheLuminance), dot(entry.faces[4], kGiCacheLuminance), dot(entry.faces[5], kGiCacheLuminance),
			GiCacheValid(entry) ? 1.0 : 0.0, surface ? 1.0 : 0.0, fresh ? 1.0 : 0.0, active ? 1.0 : 0.0, 0.0,
			voxelNormal.x, voxelNormal.y, voxelNormal.z, float(GiVoxelFaces(voxel)), float(voxel.albedo >> 24), float(rayCount),
			(voxel.emissionB & kGiVoxelDoubleSided) != 0u ? 1.0 : 0.0, float(gi.clip)};
		for (uint valueIt = 0; valueIt < 30; valueIt++)
			if (valueIt < 2u || valueIt > 4u)
				if (valueIt != 21u)
					gGiStatistics[gPushConstants.frameIndex][dumpFirst + valueIt] = asuint(values[valueIt]);
	}
}

// a voxel's light as the filter takes it (see ComputeGiCacheFilter): its faces, the variance of its light along its
// open side (of the mean: the samples' over their count, then as the filter's passes average it), the open side, and
// how much of the voxel's light is the cache's (0: none, it has none of its own: no samples, or inside something)
struct GiCacheTap
{
	float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	float variance;
	float confidence;
	float3 normal;
};

GiCacheTap GiCacheNoTap()
{
	GiCacheTap tap;
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		tap.faces[faceIt] = 0.0;
	tap.variance = 0.0;
	tap.confidence = 0.0;
	tap.normal = 0.0;
	return tap;
}

// a filter pass's record of a voxel (gGiCacheRecords, see GiCacheRecordIndex): the faces as an entry's, then the variance,
// the confidence and the open side
GiCacheTap GiCacheLoadTap(uint index)
{
	uint4 words[3] = {gGiCacheRecords[index * 3u], gGiCacheRecords[index * 3u + 1u], gGiCacheRecords[index * 3u + 2u]};
	GiCacheTap tap;
	GiCacheUnpackFaces(words, tap.faces);
	tap.variance = asfloat(words[2].y);
	tap.confidence = asfloat(words[2].z);
	tap.normal = GiCacheUnpackNormal(words[2].w);
	return tap;
}

void GiCacheStoreTap(uint index, GiCacheTap tap)
{
	uint packed[9];
	GiCachePackFaces(tap.faces, packed);
	gGiCacheRecords[index * 3u] = uint4(packed[0], packed[1], packed[2], packed[3]);
	gGiCacheRecords[index * 3u + 1u] = uint4(packed[4], packed[5], packed[6], packed[7]);
	gGiCacheRecords[index * 3u + 2u] = uint4(packed[8], asuint(tap.variance), asuint(tap.confidence), GiCachePackNormal(tap.normal));
}

// an entry as the filter takes it unfiltered: in a block the filter doesn't run in (not active, see ComputeGiResolve),
// with its own samples' variance
GiCacheTap GiCacheTapOf(GiCacheEntry entry)
{
	if (!GiCacheValid(entry))
		return GiCacheNoTap();
	GiCacheTap tap;
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		tap.faces[faceIt] = entry.faces[faceIt];
	float n = float(entry.count);
	float light = GiCacheLight(entry.faces, entry.normal);
	tap.variance = (n * max(entry.moment - light * light, 0.0) + kGiCacheVariancePrior * entry.moment) / (n + kGiCacheVariancePrior) / min(n, kGiCacheFilterCount);
	tap.confidence = n / (n + kGiCachePrior);
	tap.normal = entry.normal;
	return tap;
}

// a voxel as a filter pass reads it: the pass before's record (in that half of the records) where the filter runs (an
// active block), the entry itself where it doesn't, none in a block nothing is near or without a page
GiCacheTap GiCacheReadTap(GiData gi, uint half, uint3 at, uint3 blocks)
{
	uint blockIndex = GiCacheBlockIndex(at, blocks);
	uint flags = gGiFlags[GiFlagIndex(gi, blockIndex)];
	if ((flags & 1u) == 0u)
		return GiCacheNoTap();
	if ((flags & SHADER_TYPES_GI_FLAG_CACHE_ACTIVE) == 0u)
	{
		uint page = GiCachePageOf(gGiCacheBlocks[GiCacheTableWord(gi.cacheTableFirst, blockIndex)]);
		return page != 0u ? GiCacheTapOf(GiCacheLoad(GiCacheEntryIndex(page, at))) : GiCacheNoTap();
	}
	return GiCacheLoadTap(GiCacheRecordIndex(gi, half, gGiCacheBlocks[gi.cacheRecordTableFirst + blockIndex], at));
}

// how much two voxels' open sides agree, for the filter: to the 8th, so that a floor's voxels don't take a wall's, nor a
// thin wall's two sides each other's
float GiCacheAgreement(float3 a, float3 b)
{
	float agreement = saturate(dot(a, b));
	agreement *= agreement;
	agreement *= agreement;
	return agreement * agreement;
}

// per voxel of a clip near a surface, after the resolve (which updated the cache and wrote the probes' light): the
// cache's light filtered with its neighbours' on the same surface, as SVGF filters a frame (Schied et al. 2017; giStep
// is the pass). the first pass estimates each voxel's noise: its samples' variance pooled with its neighbours' (the
// 3x3x3 on its surface, by their samples), over its own samples. a voxel's own variance was no measure of it: under a
// source smaller than its rays' spacing (the sky under Sponza's curtains) the voxel that happened to miss it measured
// none and took none of a lucky neighbour's light, the lucky one measured plenty and took the others', and the light
// along the curtains broke into dim blobs. then GiData::cacheFilterPassCount passes of an a-trous filter, its taps 3 a
// side, 1, 2, 4, ... voxels apart (3 passes reach 7 voxels each way): each tap by the kernel's weight (1/4, 1/2, 1/4 an
// axis), how much its open side agrees with the voxel's (GiCacheAgreement), how far it is off the voxel's surface along
// its open side (a gaussian of a voxel's deviation), its confidence, and how far its light is from the voxel's against
// what both their noise allows (exp(-difference / cacheSigma deviations of the difference)): noise is averaged out, and
// an edge in the light brighter than the noise (the sky's along the curtains' lower edges) stays sharp. both, so that two
// voxels take each other's light alike: by the voxel's noise alone, a noisy bright voxel took its quiet dark neighbours'
// light and they refused its, and the light under the curtains lost a quarter (the floor's mean 0.0130 at exposure 7
// against 0.0169 pooled without the test, 0.0146 now). the variance follows the passes (the weights' squares over their
// sum's). the last pass moves the voxel's light towards the filtered light by its confidence (GiData::cacheShow: all of
// it); the passes before write the active blocks' records (see GiCacheTakeRecord), to gGiCacheRecords' halves in turn. a
// group per block of 4x4x4 voxels, as the resolve's
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiCacheFilter(uint3 groupId: SV_GroupID, uint groupIndex: SV_GroupIndex)
{
	GiData gi = GiClip(gPushConstants.giClip);
	if (gi.cacheRayCount == 0u)
		return;
	uint pass = gPushConstants.giStep;
	uint3 size = GiLevelSize(gi, 0);
	uint3 blocks = GiLevelSize(gi, SHADER_TYPES_GI_FLAG_LEVEL);
	uint blockIndex = groupId.y * (SHADER_TYPES_GI_ROW / SHADER_TYPES_GI_GROUP_SIZE) + groupId.x;
	if (blockIndex >= blocks.x * blocks.y * blocks.z)
		return;
	uint blockFlags = gGiFlags[GiFlagIndex(gi, blockIndex)];
	if ((blockFlags & 1u) == 0u)
		return;
	uint3 block = uint3(blockIndex % blocks.x, (blockIndex / blocks.x) % blocks.y, blockIndex / (blocks.x * blocks.y));
	uint3 at = block * 4u + uint3(groupIndex & 3u, (groupIndex >> 2) & 3u, groupIndex >> 4);
	if (any(at >= size))
		return;
	bool last = pass >= gi.cacheFilterPassCount;
	// a block no lookup read lately (see ComputeGiResolve) only takes its own light, in the last pass
	bool active = (blockFlags & SHADER_TYPES_GI_FLAG_CACHE_ACTIVE) != 0u;
	if (!active && !last)
		return;
	uint target = active ? GiCacheRecordIndex(gi, pass % 2u, gGiCacheBlocks[gi.cacheRecordTableFirst + blockIndex], at) : 0u;
	if (pass == 0u)
	{
		uint page = GiCachePageAt(gi, at, blocks);
		GiCacheEntry entry = page != 0u ? GiCacheLoad(GiCacheEntryIndex(page, at)) : GiCacheEmpty();
		if (!GiCacheValid(entry))
		{
			GiCacheStoreTap(target, GiCacheNoTap());
			return;
		}
		// the samples' mean and mean square around the voxel, by their counts (each neighbour's light read along the
		// voxel's open side, its mean square along its own, which agrees)
		float mean = 0.0;
		float square = 0.0;
		float samples = 0.0;
		for (int z = -1; z <= 1; z++)
			for (int y = -1; y <= 1; y++)
				for (int x = -1; x <= 1; x++)
				{
					int3 offset = int3(x, y, z);
					int3 neighbour = int3(at) + offset;
					if (!GiInside(neighbour, size))
						continue;
					uint otherPage = GiCachePageAt(gi, uint3(neighbour), blocks);
				GiCacheEntry other = (x == 0 && y == 0 && z == 0) ? entry
								   : otherPage != 0u ? GiCacheLoad(GiCacheEntryIndex(otherPage, uint3(neighbour))) : GiCacheEmpty();
					if (!GiCacheValid(other))
						continue;
					float plane = dot(float3(offset), entry.normal);
					float weight = GiCacheAgreement(other.normal, entry.normal) * exp(-0.5 * plane * plane) * float(other.count);
					mean += weight * GiCacheLight(other.faces, entry.normal);
					square += weight * other.moment;
					samples += weight;
				}
		mean /= samples;
		square /= samples;
		float n = float(entry.count);
		GiCacheTap tap;
		for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
			tap.faces[faceIt] = entry.faces[faceIt];
		tap.variance = (samples * max(square - mean * mean, 0.0) + kGiCacheVariancePrior * square) / (samples + kGiCacheVariancePrior) / min(n, kGiCacheFilterCount);
		tap.confidence = n / (n + kGiCachePrior);
		tap.normal = entry.normal;
		GiCacheStoreTap(target, tap);
		return;
	}
	uint source = (pass - 1u) % 2u;
	GiCacheTap self = GiCacheReadTap(gi, source, at, blocks);
	if (self.confidence <= 0.0)
	{
		if (!last)
			GiCacheStoreTap(target, self);
		return;
	}

	int step = 1 << (pass - 1u);
	float light = GiCacheLight(self.faces, self.normal);
	float3 filtered[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		filtered[faceIt] = 0.0;
	float weights = 0.0;
	float variance = 0.0;
	int reach = active ? 1 : 0;
	for (int z = -reach; z <= reach; z++)
		for (int y = -reach; y <= reach; y++)
			for (int x = -reach; x <= reach; x++)
			{
				int3 offset = int3(x, y, z) * step;
				int3 neighbour = int3(at) + offset;
				if (!GiInside(neighbour, size))
					continue;
				GiCacheTap tap = (x == 0 && y == 0 && z == 0) ? self : GiCacheReadTap(gi, source, uint3(neighbour), blocks);
				if (tap.confidence <= 0.0)
					continue;
				float kernel = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25) * (z == 0 ? 0.5 : 0.25);
				float plane = dot(float3(offset), self.normal);
				float difference = abs(GiCacheLight(tap.faces, self.normal) - light);
				float weight = kernel * GiCacheAgreement(tap.normal, self.normal) * exp(-0.5 * plane * plane) * tap.confidence
								 * exp(-difference / (gi.cacheSigma * sqrt(max(self.variance + tap.variance, 0.0)) + 1e-6));
				for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
					filtered[faceIt] += weight * tap.faces[faceIt];
				weights += weight;
				variance += weight * weight * tap.variance;
			}
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		filtered[faceIt] /= weights;
	if (!last)
	{
		GiCacheTap record;
		for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
			record.faces[faceIt] = filtered[faceIt];
		record.variance = variance / (weights * weights);
		record.confidence = self.confidence;
		record.normal = self.normal;
		GiCacheStoreTap(target, record);
		return;
	}

	uint3 texel = uint3(at.x, at.y, at.z + gi.volumeImage.w);
	float4 volumes[SHADER_TYPES_GI_VOLUME_COUNT];
	for (uint volumeIt = 0; volumeIt < SHADER_TYPES_GI_VOLUME_COUNT; volumeIt++)
		volumes[volumeIt] = gGiVolumeImages[volumeIt][texel];
	float seen = volumes[4].w;
	// per color its faces +x, -x, +y and -y, then red's and green's +z and -z, then blue's and the weight (see GiSample)
	float3 sh[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	for (uint faceIt = 0; faceIt < 4; faceIt++)
		sh[faceIt] = float3(volumes[0][faceIt], volumes[1][faceIt], volumes[2][faceIt]);
	sh[4] = float3(volumes[3].x, volumes[3].z, volumes[4].x);
	sh[5] = float3(volumes[3].y, volumes[3].w, volumes[4].y);
	// the voxel's weight in the lookups (see GiSampleClip) is the cache's confidence where the probes see it less: its
	// light is the voxel's own. premultiplied by the probes' weight alone, a voxel few probes see (a ten thousandth: the
	// last clip's probes 3.7 m apart at resolution 64 don't see into Sponza's corridor) held the cache's light in half
	// floats' subnormals, each color rounded its own way, and the corridor came out in pink, yellow and blue blotches;
	// and a voxel no probe sees took none of it
	float confidence = gi.cacheShow != 0u ? 1.0 : self.confidence;
	float weight = max(seen, confidence);
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		sh[faceIt] = lerp(seen > 0.0 ? sh[faceIt] / seen : filtered[faceIt], filtered[faceIt], confidence) * weight;
	seen = weight;
	gGiVolumeImages[0][texel] = float4(sh[0].r, sh[1].r, sh[2].r, sh[3].r);
	gGiVolumeImages[1][texel] = float4(sh[0].g, sh[1].g, sh[2].g, sh[3].g);
	gGiVolumeImages[2][texel] = float4(sh[0].b, sh[1].b, sh[2].b, sh[3].b);
	gGiVolumeImages[3][texel] = float4(sh[4].r, sh[5].r, sh[4].g, sh[5].g);
	gGiVolumeImages[4][texel] = float4(sh[4].b, sh[5].b, 0.0, seen);
}

// the cache's storage before the resolve (see GiCachePopPage; clip 0's GiData, a thread each of the tables' blocks or
// the pages, whichever are more): where it is made anew (GiData::cacheInit: a new pool, or a reset), the tables hold no
// page and the free list every one; else the records' count starts over, and the pages a pool that grew added
// (GiData::cacheGrowFirst and on, the ones before copied over with their entries) go on the free list
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiCachePrepare(uint3 dispatchId: SV_DispatchThreadID)
{
	uint thread = GiThread(dispatchId).x;
	GiData gi = GiClip(0);
	if (gi.cacheRayCount == 0u)
		return;
	if (gi.cacheInit != 0u)
	{
		if (thread < 5u * gi.cacheBlockCount)
			gGiCacheBlocks[thread] = 0u;
		if (thread < gi.cachePageCapacity)
			gGiCacheFree[2u + thread] = thread;
		if (thread == 0u)
		{
			gGiCacheFree[0] = gi.cachePageCapacity;
			gGiCacheFree[1] = 0u;
		}
		return;
	}
	if (thread == 0u)
		gGiCacheFree[1] = 0u;
	if (gi.cacheGrowFirst != 0u && thread >= gi.cacheGrowFirst && thread < gi.cachePageCapacity)
		GiCachePushPage(thread);
}

// the pages of the clip's blocks last frame (a thread each) that the resolve doesn't carry over (GiCacheKeeps, by the
// scroll: the block at its place now isn't near a surface or left the clip, held no entry, or wasn't read for a while) go
// back on the free list (see GiCachePopPage)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiCacheRelease(uint3 dispatchId: SV_DispatchThreadID)
{
	uint thread = GiThread(dispatchId).x;
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 blocks = GiLevelSize(gi, SHADER_TYPES_GI_FLAG_LEVEL);
	if (gi.cacheRayCount == 0u || gi.cacheInit != 0u || thread >= blocks.x * blocks.y * blocks.z)
		return;
	uint word = GiCacheTableWord(gi.cacheTablePreviousFirst, thread);
	uint page = GiCachePageOf(gGiCacheBlocks[word]);
	if (page == 0u)
		return;
	int3 block = int3(thread % blocks.x, (thread / blocks.x) % blocks.y, thread / (blocks.x * blocks.y)) - gi.scroll.xyz / int(1u << SHADER_TYPES_GI_FLAG_LEVEL);
	bool nearSurface = GiInside(block, blocks) && (gGiFlags[GiFlagIndex(gi, GiVoxelIndex(uint3(block), blocks))] & 1u) != 0u;
	if (!GiCacheKeeps(gi, gGiCacheBlocks[word], gGiCacheBlocks[word + 1u], nearSurface))
		GiCachePushPage(page - 1u);
}

// a block's probes are 8: the spacing is a multiple of 8 voxels (Renderer checks), so a block never spans two cells
groupshared float4 gGiSharedIrradiance[8 * SHADER_TYPES_GI_IRRADIANCE_COUNT];
groupshared uint gGiSharedDepth[8 * SHADER_TYPES_GI_DEPTH_SIDE * SHADER_TYPES_GI_DEPTH_SIDE];
groupshared float3 gGiSharedProbePosition[8];
// whether the probe moved since the last frame, or is new: what its voxels' traced visibility was traced from is gone
groupshared uint gGiSharedProbeMoved[8];
// the block's page of the cache (+ 1, 0 for none), whether it is new to the block, and whether a lookup read it or a
// block around it lately (see ComputeGiResolve)
groupshared uint gGiSharedCachePage;
groupshared uint gGiSharedCacheFresh;
groupshared uint gGiSharedCacheActive;

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
	uint3 texel = uint3(at.x, at.y, at.z + gi.volumeImage.w);
	// only the voxels within a few of a surface are looked up (see GiSampleSurface): the rest see no probe
	if ((gGiFlags[GiFlagIndex(gi, blockIndex)] & 1u) == 0u)
	{
		if (inside)
		{
			for (uint volumeIt = 0; volumeIt < SHADER_TYPES_GI_VOLUME_COUNT; volumeIt++)
				gGiVolumeImages[volumeIt][texel] = 0.0;
			// nothing kept: once the block is near something again, its probes are traced anew
			if (GiTracesBvh(gi))
				gGiVisibility[gi.visibilityFirst + GiVoxelIndex(at, size)] = 0u;
		}
		// nor a page of the cache's (the release freed the one it had)
		if (groupIndex == 0u && gi.cacheRayCount != 0u)
		{
			uint word = GiCacheTableWord(gi.cacheTableFirst, blockIndex);
			gGiCacheBlocks[word] = 0u;
			gGiCacheBlocks[word + 1u] = 0u;
			gGiCacheBlocks[gi.cacheRecordTableFirst + blockIndex] = 0u;
		}
		return;
	}
	// the block's page of the cache (see GiCachePopPage): the one it had last frame (at its place then, by the scroll)
	// where it keeps it (GiCacheKeeps), else, where it wanted one last frame and a lookup reads it now, a free one (fresh:
	// its entries a page's last block's), taken below; and the last frame a lookup read it
	uint cachePage = 0u;
	uint cacheWanted = 0u;
	uint cacheLastRead = 0u;
	if (gi.cacheRayCount != 0u && groupIndex == 0u)
	{
		int3 from = int3(block) + gi.scroll.xyz / int(1u << SHADER_TYPES_GI_FLAG_LEVEL);
		if (GiInside(from, blocks))
		{
			uint word = GiCacheTableWord(gi.cacheTablePreviousFirst, GiVoxelIndex(uint3(from), blocks));
			uint entry = gGiCacheBlocks[word];
			cacheLastRead = gGiCacheBlocks[word + 1u];
			cacheWanted = entry & kGiCacheWanted;
			cachePage = GiCacheKeeps(gi, entry, cacheLastRead, true) ? GiCachePageOf(entry) : 0u;
		}
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
	{
		if (inside && GiTracesBvh(gi))
			gGiVisibility[gi.visibilityFirst + GiVoxelIndex(at, size)] = 0u;
		// the cache keeps the page it had, unchanged (the release didn't free it), but takes no new one: a fresh page's
		// entries would be another block's
		if (groupIndex == 0u && gi.cacheRayCount != 0u)
		{
			uint word = GiCacheTableWord(gi.cacheTableFirst, blockIndex);
			gGiCacheBlocks[word] = cachePage | cacheWanted;
			gGiCacheBlocks[word + 1u] = cacheLastRead;
			gGiCacheBlocks[gi.cacheRecordTableFirst + blockIndex] = 0u;
			gGiFlags[GiFlagIndex(gi, blockIndex)] &= ~SHADER_TYPES_GI_FLAG_CACHE_ACTIVE;
		}
		return;
	}
	for (uint loadIt = groupIndex; loadIt < slotCount * SHADER_TYPES_GI_IRRADIANCE_COUNT; loadIt += SHADER_TYPES_GI_GROUP_SIZE)
	{
		uint slot = loadIt / SHADER_TYPES_GI_IRRADIANCE_COUNT;
		uint3 probe = uint3(clamp(blockBase + int3(slot % slots.x, (slot / slots.x) % slots.y, slot / (slots.x * slots.y)), int3(0), int3(gi.probeSize.xyz) - 1));
		gGiSharedIrradiance[loadIt] = gGiIrradiance[(gi.probeFirst + GiProbeIndex(probe, gi.probeSize)) * SHADER_TYPES_GI_IRRADIANCE_COUNT + loadIt % SHADER_TYPES_GI_IRRADIANCE_COUNT];
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
		uint current = gi.probeFirst + GiProbeIndex(probe, gi.probeSize);
		gGiSharedProbePosition[slot] = GiProbePosition(gi, probe, current);
		uint history = GiProbeHistory(gi, probe);
		gGiSharedProbeMoved[slot] = history == kGiNoProbe || any(GiProbeOffset(history) != GiProbeOffset(current)) ? 1u : 0u;
	}
	// whether the surface cache traces and filters the block this frame: a lookup read it, or one of the blocks around it
	// (the filter's reach), in the last kGiCacheSeenFrameCount frames (GiMarkSeen, at the block's place then, by the
	// scroll). the others trace a ray now and then, and the filter takes their own light (see ComputeGiCacheFilter): at
	// the user's corridor pose in Sponza, the cache traced and filtered every voxel near a surface in the 3 clips, for 4
	// ms a frame
	if (gi.cacheRayCount != 0u && groupIndex == 0u)
	{
		bool read = gi.cacheEverywhere != 0u;
		for (uint aroundIt = 0; aroundIt < 27u && !read; aroundIt++)
		{
			int3 around = int3(block) + gi.scroll.xyz / int(1u << SHADER_TYPES_GI_FLAG_LEVEL) + int3(aroundIt % 3u, (aroundIt / 3u) % 3u, aroundIt / 9u) - 1;
			uint seen = GiInside(around, blocks) ? gGiFlags[GiFlagIndex(gi, GiVoxelIndex(uint3(around), blocks)) + 2u] : 0u;
			read = seen != 0u && gi.frameStamp - seen <= kGiCacheSeenFrameCount;
		}
		bool fresh = cachePage == 0u && cacheWanted != 0u && read;
		if (fresh)
			cachePage = GiCachePopPage();
		fresh = fresh && cachePage != 0u;
		// the wanted bit starts over: the block's voxels that hold an entry set it (below)
		uint word = GiCacheTableWord(gi.cacheTableFirst, blockIndex);
		gGiCacheBlocks[word] = cachePage;
		gGiCacheBlocks[word + 1u] = read ? gi.frameStamp : cacheLastRead;
		if (cachePage != 0u || (cacheWanted != 0u && read))
			InterlockedAdd(gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CACHE_BLOCKS], 1u);
		// filtered this frame where it was read and has a record (see GiCacheTakeRecord)
		uint record = 0u;
		if (read && cachePage != 0u)
		{
			record = GiCacheTakeRecord(gi);
			InterlockedAdd(gGiStatistics[gPushConstants.frameIndex][GI_STATISTIC_CACHE_ACTIVE_BLOCKS], 1u);
		}
		gGiCacheBlocks[gi.cacheRecordTableFirst + blockIndex] = record;
		uint flagIndex = GiFlagIndex(gi, blockIndex);
		gGiFlags[flagIndex] = (gGiFlags[flagIndex] & ~SHADER_TYPES_GI_FLAG_CACHE_ACTIVE) | (record != 0u ? SHADER_TYPES_GI_FLAG_CACHE_ACTIVE : 0u);
		gGiSharedCachePage = cachePage;
		gGiSharedCacheFresh = fresh ? 1u : 0u;
		gGiSharedCacheActive = read ? 1u : 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	bool active = gi.cacheRayCount != 0u && gGiSharedCacheActive != 0u;
	uint page = gi.cacheRayCount != 0u ? gGiSharedCachePage : 0u;
	if (!inside)
		return;

	float3 center = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	GiVoxel voxel = gGiVoxels[gi.voxelFirst + GiVoxelIndex(at, size)];
	// a voxel a single sided triangle's front face wrote holds its back side, which is never drawn (outside a room's
	// walls, inside a solid): it holds no light for the lookups, though a drawn side wrote it too (a voxel can't tell
	// which of it is whose). resolved for that side, the probes there lit it, and a lookup at a surface beside it, whose
	// filter reached it, took their light: the voxel row over salle_de_bain's ceiling, a single sided triangle 0.08 m
	// under the row's centers, lit from the sky through the space over it, drew a bright line along the walls' tops
	// (0.56 against the wall's 0.13); at a wall's top, the wall's room side and the space over the ceiling share the row,
	// and its open side, by the occupied voxels below it, was up. CornellBox the same, Sponza against its reference
	// 0.390 to 0.385
	if ((voxel.albedo >> 24) != 0u && (GiVoxelFaces(voxel) & 1u) != 0u && (voxel.emissionB & kGiVoxelDoubleSided) == 0u)
	{
		for (uint volumeIt = 0; volumeIt < SHADER_TYPES_GI_VOLUME_COUNT; volumeIt++)
			gGiVolumeImages[volumeIt][texel] = 0.0;
		if (GiTracesBvh(gi))
			gGiVisibility[gi.visibilityFirst + GiVoxelIndex(at, size)] = 0u;
		if (page != 0u)
			GiCacheClear(GiCacheEntryIndex(page, at));
		return;
	}
	// which way is away from the surfaces next to it: the occupancy's gradient over its 6 neighbors, plus a surface's
	// voxel's own normal; the point the probes are tested from is out that way, by a fifth of a spacing (DDGI's normal
	// bias: a point close to a surface fails a probe's test at a grazing angle, where the probe's depth that way is of
	// rays into the surface before it and past it), at least a voxel (from a surface voxel's center, among its surface's
	// voxels, a probe saw nothing). the gradient for a surface's voxel too: by its normal alone, a floor's voxel at a
	// wall's base was tested from a point straight up, as close to the wall as before, which the sunlit probes beyond the
	// wall saw at a grazing angle (a bright band along Sponza's floors)
	// over the 3 voxels each way, the nearer ones counting more: the lookups land a voxel or two off a surface (see
	// GiSampleSurface), where the 6 neighbors alone gave no side, so every probe counted by its visibility alone, and a
	// probe behind Bistro's hotel facade, whose depth toward the street mixes the facade's with the window's beside
	// it (Chebyshev lets the point through), lit the wall by the window with the hotel's interior
	float3 away = 0.0;
	for (uint axisIt = 0; axisIt < 3; axisIt++)
	{
		int3 step = int3(axisIt == 0u, axisIt == 1u, axisIt == 2u);
		for (int reachIt = 1; reachIt <= 3; reachIt++)
		{
			float below = GiInside(int3(at) - step * reachIt, size) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(int3(at) - step * reachIt), size)].albedo >> 24) != 0u ? 1.0 : 0.0;
			float above = GiInside(int3(at) + step * reachIt, size) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(int3(at) + step * reachIt), size)].albedo >> 24) != 0u ? 1.0 : 0.0;
			away[axisIt] += (below - above) / float(reachIt);
		}
	}
	if ((voxel.albedo >> 24) != 0u)
	{
		float3 normal = GiVoxelNormal(voxel.normal);
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
	// where the rays trace the BVHs, whether each probe sees the voxel is a ray between them (see the loop below), kept:
	// the bits of the voxel at its place last frame (by the scroll), traced again where any of the block's probes moved
	// (its relocation, see ComputeGiProbeUpdate) or is new, and wherever they aren't valid: against the static BVH
	// nothing else changes them. by block, a group's threads together: refreshed a voxel in 8 a frame, a few threads of
	// every simd group traced, which cost as much as all of them, and a block in 32 a frame still cost Bistro's street
	// 2.6 ms (a group's threads trace 8 rays each in turn, and the dispatch waits for the slowest group). aimed at the
	// voxel's center, a surface's voxel's out along its own normal by half a voxel (the side it stands for): the open
	// side the test point above goes by is a guess at the voxels around, which in a coarse clip pointed salle_de_bain's
	// niches' points into the space behind their wall
	bool traced = GiTracesBvh(gi);
	uint voxelIndex = GiVoxelIndex(at, size);
	uint visibilityBits = 0u;
	bool retrace = false;
	float3 tracedPoint = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	if (traced)
	{
		int3 from = int3(at) + gi.scroll.xyz;
		visibilityBits = gi.reset == 0u && GiInside(from, size) ? gGiVisibility[gi.visibilityPreviousFirst + GiVoxelIndex(uint3(from), size)] : 0u;
		retrace = (visibilityBits & kGiVisibilityValid) == 0u;
		for (uint slotIt = 0; slotIt < slotCount; slotIt++)
			retrace = retrace || gGiSharedProbeMoved[slotIt] != 0u;
		if (retrace)
			visibilityBits = kGiVisibilityValid;
		// no further than halfway to a surface facing back at it (see GiOpenAlong)
		if ((voxel.albedo >> 24) != 0u)
			tracedPoint += GiVoxelNormal(voxel.normal) * (min(0.5, 0.5 * GiUnpackRgba16(gGiRadiance[gi.directFirst + voxelIndex]).a) * gi.volumeOrigin.w);
	}
	// between the block's cell's probes (the moved center may be past the cell: clamped, since only its probes are in
	// shared memory; read past them, the floor behind Sponza's fountains came out as black and colored garbage)
	int3 base = blockBase;
	fraction = saturate((center - GiProbeLatticePosition(gi, uint3(max(base, int3(0))))) / gi.probeOrigin.w);
	int3 slotBase = int3(0);
	float3 sh[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
		sh[shIt] = 0.0;
	float seen = 0.0;
	// traced, the hidden probes' light apart: the least of it (see below)
	float3 shHidden[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
		shHidden[shIt] = 1e30;
	float hiddenWeight = 0.0;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
		uint3 probe = uint3(clamp(base + offset, int3(0), int3(gi.probeSize.xyz) - 1));
		uint3 slotAt = uint3(slotBase + offset);
		uint slot = (slotAt.z * slots.y + slotAt.y) * slots.x + slotAt.x;
		float3 weights = select(offset != 0, fraction, 1.0 - fraction);
		float weight = 1.0;
		float3 probePosition = gGiSharedProbePosition[slot];
		float facingWeight = 1.0;
		float facing = 1.0;
		// the probes on the open side weigh more (wrapped, so that those beside it count a little; DDGI's fifth for the
		// ones behind let the probes below Sponza's floors, in the clipmap's margin and lit by the environment through its
		// bottom, light the floors' edges along the walls, where every probe in the corridor fails and a point takes the
		// mean of what is left)
		if (awayLength > 0.0)
		{
			facing = dot(normalize(probePosition - center), away) * 0.5 + 0.5;
			facingWeight = facing * facing + 0.02;
			weight *= facingWeight;
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
		float nearest = 1e9;
		for (uint tapIt = 0; tapIt < 4; tapIt++)
		{
			float3 tap = GiUnpackDepth(gGiSharedDepth[slot * kTexelCount + taps.texel[tapIt]]);
			moments += taps.weight[tapIt] * float2(abs(tap.x), tap.y);
			nearest = min(nearest, abs(tap.z));
		}
		// traced: the nearest thing between the probe and the voxel. a surface's back means the probe is behind it, and
		// it lights the voxel not at all, not even by the floor below (a point no probe sees takes their mean); a front
		// only hides it. exact at any spacing, where a coarse clip's depth texel (22 degrees) is half a metre wide a few
		// metres out, and mixes what is beside the point with what is in front of it: salle_de_bain's niches, 67 units
		// from the eye in the last clip, were lit by the probes behind its back wall through the window beside them
		// (white patches the surface reference hasn't). traced every frame it cost Sponza 7 ms, Bistro's street 12.6
		uint state = kGiVisible;
		if (traced)
		{
			if (retrace)
			{
				float3 toPoint = tracedPoint - probePosition;
				float pointDistance = length(toPoint);
				// short of the point by a twentieth of a voxel only: by a quarter, a surface crossed within it went untested,
				// and a probe over CornellBox's ceiling (outside the box) saw a corner voxel's point 4 mm under it, which
				// lit a streak along the ceiling's edge at the back
				BvhHit hit = GiTraceTriangles(gi, probePosition, toPoint / max(pointDistance, 1e-6), max(pointDistance - 0.05 * gi.volumeOrigin.w, 0.0), false, false, true);
				state = !BvhHitSomething(hit) && !hit.capped ? kGiVisible
						: (!BvhHitSomething(hit) || hit.frontFace || (hit.flags & BVH_TRIANGLE_FLAG_DOUBLE_SIDED) != 0u ? kGiHidden : kGiBehind);
				visibilityBits |= state << (cornerIt * 2u);
			}
			else
				state = (visibilityBits >> (cornerIt * 2u)) & 3u;
		}
		// after the trace: a probe skipped here keeps its state all the same (skipped before it, its bits stayed
		// kGiVisible, and once its inside share fell below the line again, a probe behind salle_de_bain's niches' wall,
		// hovering about it, lit them white, in some runs and not others)
		if ((!traced && moments.x < 1e-3) || gGiSharedIrradiance[slot * SHADER_TYPES_GI_IRRADIANCE_COUNT + 5].w > kGiInsideShare || state == kGiBehind)
			continue;
		float d = distance / gi.probeOrigin.w;
		float variance = max(moments.y - moments.x * moments.x, 0.0) + 1e-4;
		float visibility = 1.0;
		if (traced)
			visibility = state == kGiVisible ? 1.0 : 0.0;
		else if (d > moments.x)
		{
			float delta = d - moments.x;
			visibility = variance / (variance + delta * delta);
			visibility *= visibility * visibility;
		}
		// and a point well behind the nearest thing the texel's rays met (past one and a half times its distance and a
		// tenth of a spacing: a texel toward a floor at a grazing angle meets it nearer than the point all the way, a
		// continuum, where the point is within half again of the nearest; by a tenth of a spacing alone, those probes
		// failed too) is behind the nearer of two things the texel sees (the wall, with the window beside it: see
		// ComputeGiProbeUpdate), which Chebyshev's test can't tell: nearer than their mean it let the point through, and
		// beyond it the mix's variance did (a probe inside the building by Bistro's square, its texel toward the ground
		// before the entrance at a mean of 0.39 spacings with the wall at 0.066, saw the ground at 0.54 at 0.55, and
		// alone of its cell's probes, the rest failing, lit the square's ground and the entrance with the interior). such
		// a probe gets a tenth of the floor below too: a point every probe fails takes their mean, which one behind a
		// wall, lit by what is there, pulled its way
		float behind = !traced && nearest < 1e8 ? smoothstep(nearest * 1.5 + 0.1, nearest * 2.0 + 0.2, d) : 0.0;
		visibility *= 1.0 - behind;
		weight *= max(visibility, 0.001);
		// less of the ones seen little (DDGI's crush: below a fifth, cubed), then a little of every probe, so that a point
		// none sees takes their mean rather than nothing (the floor after the crush: a floor before it was crushed to
		// 1e-11, which the volumes' half floats hold as 0, so such a point had no light at all: black patches wherever
		// every probe of a cell failed); then by where the point is between the probes (after the crush, as RTXGI has
		// it: crushing the product crushed every probe at a cell's center, where each is an eighth, and the facing ones
		// least, which drew the cells)
		if (weight < 0.2)
			weight *= weight * weight * 25.0;
		weight = max(weight, lerp(1e-3, 1e-4, behind));
		// traced, by the facing alone, steeply (its 4th power: 1 straight ahead of the open side, a sixteenth beside it,
		// nothing behind), a thousandth of it for a probe hidden. a niche of a coarse clip's voxel no probe sees into:
		// with the wrap, the crush and the floor, the mean of all 8 was the two behind its back wall, by the window
		// outside, 40 times brighter than the room's, and where one of them saw the niche's air through the window
		// beside it, at a facing of a fiftieth, its crushed weight outweighed the hidden ones' floors in front
		// (salle_de_bain's niches lit white)
		if (traced)
			weight = (state == kGiVisible ? 1.0 : 1e-3) * facing * facing * facing * facing;
		weight *= weights.x * weights.y * weights.z;
		// a probe holding anything that isn't finite is left out (one poisoned every voxel of its cells)
		bool finite = true;
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
			finite = finite && !any(isnan(gGiSharedIrradiance[slot * SHADER_TYPES_GI_IRRADIANCE_COUNT + shIt].rgb)) && !any(isinf(gGiSharedIrradiance[slot * SHADER_TYPES_GI_IRRADIANCE_COUNT + shIt].rgb));
		if (!finite)
			continue;
		if (traced && state == kGiHidden)
		{
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
				shHidden[shIt] = min(shHidden[shIt], gGiSharedIrradiance[slot * SHADER_TYPES_GI_IRRADIANCE_COUNT + shIt].rgb);
			hiddenWeight += weight;
		}
		else
		{
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
				sh[shIt] += weight * gGiSharedIrradiance[slot * SHADER_TYPES_GI_IRRADIANCE_COUNT + shIt].rgb;
		}
		seen += weight;
		// the resolve at the dumped point's voxel, per probe (see GI_DUMP_FLOAT_COUNT): its position, the facing weight,
		// the visibility, the distance in spacings, the texel's mean and nearest, and the weight it ends with
		if (gi.dumpProbe != ~0u && all(int3(at) == int3(floor(GiVoxelCoordinate(gi, gi.dumpPoint.xyz, 0)))))
		{
			uint dumpFirst = GI_STATISTIC_COUNT + gi.clip * GI_DUMP_FLOAT_COUNT + 16 + kTexelCount + 99 + cornerIt * 10;
			float values[10] = {probePosition.x, probePosition.y, probePosition.z, facingWeight, visibility, d, moments.x, nearest, weight, gGiSharedIrradiance[slot * SHADER_TYPES_GI_IRRADIANCE_COUNT].r};
			for (uint valueIt = 0; valueIt < 10; valueIt++)
				gGiStatistics[gPushConstants.frameIndex][dumpFirst + valueIt] = asuint(values[valueIt]);
		}
	}
	// per color its faces +x, -x, +y and -y, then red's and green's +z and -z, then blue's and the weight (see GiSample)
	if (traced)
	{
		// the hidden probes by the least light of theirs (per face and color), at their weights: a voxel no probe sees can
		// confirm none of their light, and a pocket no probe sees into is a shadowed one. by their light, weighed by their
		// facing (or by how far from the voxel what hides each is), the probes behind salle_de_bain's niches' back wall,
		// in the space the window's emitter lights, straight along the niches' open side and 40 times as bright as the
		// room's, lit the niches white: what hides a probe can't tell a wall between from a niche's lip
		if (hiddenWeight > 0.0)
		{
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
				sh[shIt] += hiddenWeight * shHidden[shIt];
		}
		gGiVisibility[gi.visibilityFirst + voxelIndex] = visibilityBits;
		// a voxel no probe sees, whose probes all face it little, sums weights the volumes' half floats can't hold (a
		// thousandth of the facing's 4th power each, above): raised to what they hold, its light as it is, still far
		// below a seen voxel's
		const float kLeastSeen = 1e-4;
		if (seen > 0.0 && seen < kLeastSeen)
		{
			float scale = kLeastSeen / seen;
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; shIt++)
				sh[shIt] *= scale;
			seen = kLeastSeen;
		}
	}
	// the surface cache where it is on (its light is blended in after, see ComputeGiCacheFilter): the voxels a surface is
	// within 3 of (the lookups land a voxel or two off a surface, see GiSampleSurface); the others keep nothing
	// a voxel that holds an entry marks its block as wanting a page (see GiCacheKeeps): a block within the flags' reach
	// of a surface but none of whose voxels hold one gives its page back next frame, and one without gets one then
	bool cacheEntry = awayLength > 0.0 || (voxel.albedo >> 24) != 0u;
	if (gi.cacheRayCount != 0u && cacheEntry)
	{
		uint word = GiCacheTableWord(gi.cacheTableFirst, blockIndex);
		if ((gGiCacheBlocks[word] & kGiCacheWanted) == 0u)
			InterlockedOr(gGiCacheBlocks[word], kGiCacheWanted);
	}
	if (page != 0u)
	{
		if (cacheEntry)
			GiCacheUpdate(gi, at, size, blockIndex, page, gGiSharedCacheFresh != 0u, center, away, (voxel.albedo >> 24) != 0u, active);
		else
			GiCacheClear(GiCacheEntryIndex(page, at));
	}
	gGiVolumeImages[0][texel] = float4(sh[0].r, sh[1].r, sh[2].r, sh[3].r);
	gGiVolumeImages[1][texel] = float4(sh[0].g, sh[1].g, sh[2].g, sh[3].g);
	gGiVolumeImages[2][texel] = float4(sh[0].b, sh[1].b, sh[2].b, sh[3].b);
	gGiVolumeImages[3][texel] = float4(sh[4].r, sh[5].r, sh[4].g, sh[5].g);
	gGiVolumeImages[4][texel] = float4(sh[4].b, sh[5].b, 0.0, seen);
}

// the irradiance along the axes around a point (an ambient cube, see GiIrradianceBasis), from its clips' voxels (see
// GiSampleAt), as the volumes hold them (see ComputeGiResolve): per color its faces +x, -x, +y and -y, then red's and
// green's +z and -z, then blue's with the weight of the probes they saw. vectors rather than arrays: in the main pass's
// shader, arrays of the values spilled (Bistro's main pass 31 ms instead of 4)
struct GiSample
{
	float4 r; // along +x, -x, +y, -y
	float4 g;
	float4 b;
	float4 z; // red along +z and -z, then green
	float4 last; // blue along +z and -z, unused, and the weight
	bool valid;
	bool inside; // whether a clip holds the point (though none of its voxels around it saw a probe, if not valid)
};

// a clip's voxels' resolved probes around the point offset along its normal (see GiIrradianceResolved), normalized by
// their weight; bias is in the clip's own voxels
GiSample GiSampleClip(GiClipView gi, float3 position, float3 normal, float3 bias)
{
	GiSample result;
	float3 size = float3(gi.volumeSize.xyz);
	SamplerState linear = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	// in the clip's own voxels: in the finest clip's, a coarse clip's lookup at a thin wall landed within the filter's
	// reach of the wall's outer shell voxel, resolved from a point outside, where a probe outside the building passes
	// (the end wall of Sponza's corridor, lit white by the sky beyond it, with one clip dark)
	float3 biased = position + bias * gi.volumeOrigin.w;
	// the clip's slab of the stacked volumes, clamped inside it (no filtering across into the next)
	float3 coordinate = (biased - gi.volumeOrigin.xyz) / gi.volumeOrigin.w;
	coordinate.z = clamp(coordinate.z, 0.5, size.z - 0.5) + float(gi.volumeImage.w);
	float3 uvw = coordinate / float3(gi.volumeImage.xyz);
	result.last = gGiVolumes[4].SampleLevel(linear, uvw, 0.0);
	float weight = result.last.w;
	result.valid = weight > 1e-7;
	float scale = result.valid ? 1.0 / weight : 0.0;
	result.r = gGiVolumes[0].SampleLevel(linear, uvw, 0.0) * scale;
	result.g = gGiVolumes[1].SampleLevel(linear, uvw, 0.0) * scale;
	result.b = gGiVolumes[2].SampleLevel(linear, uvw, 0.0) * scale;
	result.z = gGiVolumes[3].SampleLevel(linear, uvw, 0.0) * scale;
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
	none.r = 0.0;
	none.g = 0.0;
	none.b = 0.0;
	none.z = 0.0;
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
		result.r += share * sample.r;
		result.g += share * sample.g;
		result.b += share * sample.b;
		result.z += share * sample.z;
		result.last += share * sample.last;
		covered += share;
	}
	if (covered <= 0.0)
		return none;
	float scale = 1.0 / covered;
	result.r *= scale;
	result.g *= scale;
	result.b *= scale;
	result.z *= scale;
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

// where the surface cache is on, the blocks a surface's lookup reads (GiSampleSurface's point, in each clip that lights
// it) stamped with the frame in their flags' third word (ComputeMain, a pixel at a time): the cache traces and filters
// around what was seen (see ComputeGiResolve)
void GiMarkSeen(float3 position, float3 normal, float3 view)
{
	if (gGi[gPushConstants.frameIndex][0].enabled == 0 || gGi[gPushConstants.frameIndex][0].cacheRayCount == 0u)
		return;
	float3 bias = normal * 0.75 + view * 1.5;
	uint clipCount = gGi[gPushConstants.frameIndex][0].clipCount;
	float remaining = 1.0;
	for (uint clipIt = 0; clipIt < clipCount && remaining > 0.0; clipIt++)
	{
		GiClipView clip = GiClipViewAt(clipIt);
		float weight = GiClipWeight(clip, position);
		if (weight <= 0.0)
			continue;
		remaining -= remaining * weight;
		float3 coordinate = (position + bias * clip.volumeOrigin.w - clip.volumeOrigin.xyz) / clip.volumeOrigin.w;
		int3 block = int3(floor(coordinate / float(1u << SHADER_TYPES_GI_FLAG_LEVEL)));
		uint3 blocks = GiLevelVoxels(clip.volumeSize.xyz, SHADER_TYPES_GI_FLAG_LEVEL);
		if (!GiInside(block, blocks))
			continue;
		uint index = clip.flagFirst + GiVoxelIndex(uint3(block), blocks) * SHADER_TYPES_GI_FLAG_WORD_COUNT + 2u;
		if (gGiFlags[index] != clip.frameStamp)
			gGiFlags[index] = clip.frameStamp;
	}
}

// a sample's irradiance over pi (the radiance a white Lambertian surface reflects) for a surface facing direction: the
// faces the direction's components point at, each by its component squared (see GiIrradianceBasis). along a reflection
// instead, the radiance around it in a cosine lobe (a rough reflection's)
float3 GiEvaluate(GiSample sample, float3 direction)
{
	float3 d = direction;
	float3 w = d * d;
	float3 x = d.x >= 0.0 ? float3(sample.r.x, sample.g.x, sample.b.x) : float3(sample.r.y, sample.g.y, sample.b.y);
	float3 y = d.y >= 0.0 ? float3(sample.r.z, sample.g.z, sample.b.z) : float3(sample.r.w, sample.g.w, sample.b.w);
	float3 z = d.z >= 0.0 ? float3(sample.z.x, sample.z.z, sample.last.x) : float3(sample.z.y, sample.z.w, sample.last.y);
	return max(w.x * x + w.y * y + w.z * z, 0.0);
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
		float passedT = -1e9;
		uint passedIndex = 0;
		while (GiInside(voxel, clip.volumeSize.xyz) && stepCount++ < 4096u)
		{
			uint index = clip.voxelFirst + GiVoxelIndex(uint3(voxel), clip.volumeSize.xyz);
			if ((gGiVoxels[index].albedo >> 24) != 0u)
			{
				// a back face's shell voxel passed, the surface where its twin follows (see GiOccluderHit)
				if (GiVoxelFaces(gGiVoxels[index]) == 2u)
				{
					passedT = t;
					passedIndex = index;
				}
				else
				{
					hitClip = clipIt;
					hitIndex = t - passedT < 2.0 ? passedIndex : index;
					return true;
				}
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
// voxel it meets holds (its radiance as the rays read it; its albedo; its normal); the
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
		float3 normal = GiVoxelNormal(voxel.normal);
		// facing the eye or away from it (a voxel lit from behind what the eye sees): dimmed
		return pow((normal * 0.5 + 0.5) * (dot(normal, direction) < 0.0 ? 1.0 : 0.25), 2.2);
	}
	GiData gi = GiClip(clip);
	uint local = index - gi.voxelFirst;
	return GiUnpackRgba16(gGiRadiance[gi.radianceFirst + local]).rgb;
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
			float inside = gGiIrradiance[probeIndex * SHADER_TYPES_GI_IRRADIANCE_COUNT + 5].w > kGiInsideShare ? 1.0 : 0.0;
			float3 toProbe = probePosition - point;
			float distance = length(toProbe);
			GiDepthTaps taps = GiDepthTapsAt(-toProbe / max(distance, 1e-6));
			float2 moments = 0.0;
			float nearest = 1e9;
			for (uint tapIt = 0; tapIt < 4; tapIt++)
			{
				float3 tap = GiUnpackDepth(gGiProbeDepth[gi.depthFirst + (probeIndex - gi.probeFirst) * kTexelCount + taps.texel[tapIt]]);
				moments += taps.weight[tapIt] * float2(abs(tap.x), tap.y);
				nearest = min(nearest, abs(tap.z));
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
			if (nearest < 1e8)
				visibility *= 1.0 - smoothstep(nearest * 1.5 + 0.1, nearest * 2.0 + 0.2, d);
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
		float3 w = normal * normal;
		float3 result = 0.0;
		for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		{
			int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
			uint3 probe = uint3(clamp(base + offset, int3(0), int3(gi.probeSize.xyz) - 1));
			float3 weights = select(offset != 0, fraction, 1.0 - fraction);
			uint first = (gi.probeFirst + GiProbeIndex(probe, gi.probeSize)) * SHADER_TYPES_GI_IRRADIANCE_COUNT;
			float3 irradiance = w.x * (normal.x >= 0.0 ? gGiIrradiance[first + 0].rgb : gGiIrradiance[first + 1].rgb) +
				w.y * (normal.y >= 0.0 ? gGiIrradiance[first + 2].rgb : gGiIrradiance[first + 3].rgb) +
				w.z * (normal.z >= 0.0 ? gGiIrradiance[first + 4].rgb : gGiIrradiance[first + 5].rgb);
			result += weights.x * weights.y * weights.z * max(irradiance, 0.0);
		}
		return result;
	}
	return -1.0;
}
