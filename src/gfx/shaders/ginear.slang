// the indirect lighting's near field: what a pixel's surface sees within a few voxels of it, which the volume's voxels
// can't hold. where a sphere rests on a floor, the floor's voxels at the contact are also the sphere's never drawn inside,
// which gets no light (see ComputeGiResolve), and the gap beside it is thinner than a voxel, all its surfaces lit by the
// gap's own unlit voxels: the volume's lookups there find no light, in blocks a voxel wide (the user's CornellBox poses,
// black rectangles under the spheres). per pixel of the pre-pass's depth, a ray from the surface through the BVHs, as far
// as GiData::nearReach voxels of the clips that light the point (GI Near, ComputeGiNear), its direction one of 16 over
// the 4x4 tile it is in (cosine distributed): the radiance of what it meets (the voxels' there, see GiHitLitAt), or
// where it meets nothing, the volume's light along it from its end; filtered over the pixels around on the same surface
// (GI Near X and Y, ComputeGiNearX and Y): the light the pixel gathers, and the share of its rays that met nothing near;
// upsampled to every pixel and accumulated over frames there (GI Near Temporal), which the main pass's opaque fragments
// take in place of the volume's lookup by how much is near (GiNearIrradiance). a part of shaders.slang, which includes
// it after tracedshadows.slang

// the size of the voxels of the clips that light a point, as the shading blends them (see GiSampleAt); 0 outside them
float GiNearVoxelSize(float3 position)
{
	uint clipCount = gGi[gPushConstants.frameIndex][0].clipCount;
	float remaining = 1.0;
	float size = 0.0;
	for (uint clipIt = 0; clipIt < clipCount && remaining > 0.0; clipIt++)
	{
		GiClipView clip = GiClipViewAt(clipIt);
		float weight = GiClipWeight(clip, position);
		if (weight <= 0.0)
			continue;
		float share = remaining * weight;
		remaining -= share;
		size += share * clip.volumeOrigin.w;
	}
	return remaining < 1.0 ? size / (1.0 - remaining) : 0.0;
}

// a traced pixel's ray (every stride-th pixel each way, GiData::nearStride, see ComputeGiNear): one of 16 directions
// over the 4x4 tile of traced pixels it is in, cosine distributed about the normal (a point of the unit disk lifted onto
// the hemisphere, Malley's method): the disk's points as the traced shadows' (ShadowDiskPoint, every row and column of
// a tile all four bands of the radius and quarters of the circle), turned per tile by a hash of its own. accumulated over
// frames (turn: the frame, see GiData::nearTemporal), each frame's are another set: shifted within the tile, so that each
// pixel takes all 16 over 16 frames, and turned by another hash (fixed, the rays' grain was fixed to the screen, which
// moved over the scene with the camera)
float3 GiNearDirection(int2 pixel, float3 normal, uint turn, uint stride)
{
	float2 disk = ShadowDiskPoint((pixel / int(stride)) + int2(turn & 3u, (turn >> 2) & 3u), 0x9e3779b9u + turn * 0x632be5abu);
	float3 t;
	float3 b;
	ShadowBasis(normal, t, b);
	return normalize(disk.x * t + disk.y * b + sqrt(max(1.0 - dot(disk, disk), 0.0)) * normal);
}

// which pixel of each stride's square is traced this frame (ComputeGiNear, the filter's passes, GiNearUpsample): the
// first, but the final gather's (GiData::nearGather) another every frame, in an order that puts a frame's far from the
// last's (a Bayer matrix's, as the reflections' ReflectionOffset), so that every pixel has rays of its own within 4
// frames at a stride of 2 and 16 at 4. on a fixed grid a pixel between the traced ones only ever took its neighbours',
// and where those are other surfaces every time (a blind's slats a few pixels apart, the bathroom's of the glTF
// research set) what it took depended on where it lay among them: a moiré over the blind, fixed to the screen
int2 GiNearOffset(GiData gi)
{
	static const uint kOrder4[16] = { 0u, 10u, 2u, 8u, 5u, 15u, 7u, 13u, 1u, 11u, 3u, 9u, 4u, 14u, 6u, 12u };
	static const uint kOrder2[4] = { 0u, 3u, 1u, 2u };
	if (gi.nearGather == 0u || gi.nearTemporal == 0u)
		return int2(0, 0);
	if (gi.nearStride == 4u)
	{
		uint index = kOrder4[gi.nearFrame & 15u];
		return int2(index & 3u, index >> 2u);
	}
	if (gi.nearStride == 2u)
	{
		uint index = kOrder2[gi.nearFrame & 3u];
		return int2(index & 1u, index >> 1u);
	}
	return int2(0, 0);
}

// a pixel's ray (see ComputeGiNear): rgb its radiance, a 0 where it met something nearer than a contact
// (GiData::nearContact), else how far the volume's lookup at the pixel is to be taken (GiLookupTrust), -1 where there is
// nothing to know (no surface, outside the clips, or the volume's lookup at its end saw no probe). the filter and the
// frames spread a as they do the light, over the pixels around on the same surface, so the shading hands over to the
// near field's light smoothly where the lookups fail (see GiNearIrradiance)
static const float4 kGiNearUnknown = float4(0.0, 0.0, 0.0, -1.0);

float4 GiNearRay(int2 pixel)
{
	ViewData view;
	ShadowSurface surface;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, surface))
		return kGiNearUnknown;
	GiData gi = GiClip(0);
	// the final gather (GiData::nearGather): the same ray as far as there is anything to meet, whose light the shading
	// takes whole (see Shade), so nothing is asked of the volume's lookup at the pixel
	bool gather = gi.nearGather != 0u;
	if (gi.enabled == 0u || (gi.nearReach <= 0.0 && !gather) || !GiTracesBvh(gi))
		return kGiNearUnknown;
	float voxel = GiNearVoxelSize(surface.position);
	if (voxel <= 0.0)
		return kGiNearUnknown;
	float3 direction = GiNearDirection(pixel, surface.normal, gi.nearTemporal != 0u ? gi.nearFrame : 0u, max(gi.nearStride, 1u));
	// off the surface by a pixel and a half (more far from the eye), as the traced shadows' rays, against the depth's
	// and the BVH's rounding
	float bias = max(1.5 * surface.metersPerPixel, 1e-4 * distance(surface.position, view.eyePosition.xyz));
	float4 ray = GiNearTrace<kGiHitThorough>(gi, surface.position, surface.normal, direction, bias, voxel, gather);
	if (ray.a > 0.0 && !gather)
	{
		bool moving;
		GiSample lookup = GiSampleSurface(surface.position, surface.normal, normalize(view.eyePosition.xyz - surface.position), GiSeenNone(), false, moving);
		ray.a *= GiLookupTrust(lookup, moving);
	}
	return ray;
}

// a near field ray from a surface at position (its normal toward the side the ray goes) along direction, off it by bias,
// in the clips whose voxels are voxel big there: as GiNearRay's (the traced reflections' and refractions' hits trace
// them too, see TracedNearIrradiance). kTests: how the voxels its hits take light from are tested (GiHitVoxel's mask: the main
// pass's, GiNearRay). gather (the final gather's, GiData::nearGather): as far as there is anything to meet, and where there
// is nothing the environment along the ray, as the probes' and the cache's rays take it (prefiltered: a sharp sky's
// bright spots are a few rays')
static const float kGiGatherReach = 60000.0;

float4 GiNearTrace<let kTests : int>(GiData gi, float3 position, float3 normal, float3 direction, float bias, float voxel, bool gather = false)
{
	float reach = gather ? kGiGatherReach : gi.nearReach * voxel;
	float3 origin = position + normal * bias;
	BvhHit hit = GiTraceTriangles(gi, origin, direction, reach, false, true, false);
	if (BvhHitSomething(hit))
	{
		// (the gather's: 1 where what the ray met is known, 0 where it isn't, see below)
		float clear = gather || hit.t >= gi.nearContact * voxel ? 1.0 : 0.0;
		// a single sided surface seen from behind: the ray is inside what it bounds, where no light is. the gather's: not
		// known, as the surface cache's rays leave such a hit out (GiCacheUpdate)
		if (!hit.frontFace && (hit.flags & BVH_TRIANGLE_FLAG_DOUBLE_SIDED) == 0u)
			return float4(0.0, 0.0, 0.0, gather ? 0.0 : clear);
		if (gi.nearShow == 4u)
			return float4(0.0, 0.0, 0.0, clear);
		// the radiance of what it met, the voxels' there (see GiHitLitAt). within a voxel and a half of the pixel's
		// tangent plane, the voxels at the hit are the pixel's surface's too (a contact, a corner), holding whichever's
		// light won them (the floor's, lit, under a sphere resting on it): read a voxel and a half off the plane instead,
		// along the hit's surface (the pixel's normal in its tangent plane), as far as it takes to get there (the reads
		// half a voxel and a voxel along the hit's normal then stay half a voxel off the plane at least). a surface facing
		// the pixel's that doesn't get there within 3 voxels that way (a sphere's underside near the floor it rests on) is
		// in a gap thinner than the voxels, which hold neither side's light there, and which little light reaches: none.
		// moved a voxel and a half whatever the slope, as it was, the reads under CornellBox's spheres crossed the floor
		// into its underside's voxels, lit by the gap between it and the box under it, a bright crescent around each
		// sphere's contact; and reading the floor's own voxel there instead, the sun's patch beside a sphere lit its
		// contact
		float3 point = origin + direction * hit.t;
		float needed = max(1.5 * voxel - dot(point - position, normal), 0.0);
		// (the gather's rays, whose light is all of the pixel's: what faces it across such a gap reflects what the lights
		// and the emitters give it at the hit itself, GiHitLitAt's bounce off, and none of what its voxel holds bounced.
		// with none at all, the slats of the bathroom's blind in the glTF research set, a few centimetres apart before an
		// emissive panel, were lit by the few rays that pass between them into the room: grey where the reference has them
		// white, the scene 0.21 stops under its reference for the lookups' 0.07; and with the hit's voxel read as any
		// other's, the gaps between the vanity's drawers lit up)
		bool gap = false;
		if (needed > 0.0)
		{
			float3 along = normal - hit.normal * dot(normal, hit.normal);
			float alongLength = length(along);
			float move = alongLength > 1e-3 ? needed / alongLength : 3.0 * voxel + 1.0;
			if (move > 3.0 * voxel)
			{
				if (dot(hit.normal, normal) < 0.0)
				{
					if (!gather)
						return float4(0.0, 0.0, 0.0, clear);
					gap = true;
				}
				move = 3.0 * voxel;
			}
			if (alongLength > 1e-3 && !gap)
				point += along * (move / alongLength);
		}
		// the light it reflects alone (GiHitLitAt): an emitter's is the volume's, which averages it over its probes'
		// rays and frames. a small one met by one ray in four pixels is a firefly: Bistro's lantern bulbs, a few
		// centimetres of 3.5, speckled the walls around them white, where they give a hundredth of the daylight
		// and its lights' light taken at the point itself, by a shadow ray from it (GiHitLitAt, as the surface cache's rays'
		// hits): a voxel's own takes it from a voxel out along its open side, which the sun coming down through
		// MovingInterior's window lit higher up the walls than their sunlit band reaches (by about a fifth of the room's
		// height), and the rays from either wall by the corner, meeting the other at once, read those voxels: a bright
		// line up the corner over the band, 30 of 255 above the walls beside it (the user's report, 2026-11-20)
		uint seed = (asuint(direction.x) * 0x9E3779B1u) ^ (asuint(direction.y) * 0x85EBCA6Bu) ^ (asuint(direction.z) * 0xC2B2AE35u);
		seed ^= seed >> 15;
		seed *= 0x2C1B3C6Du;
		seed ^= seed >> 12;
		float3 emission;
		// (the voxel read at the point moved off the corner, the shadow ray from the hit: from the moved point, which along
		// a surface sloping into the corner can pass the other surface, the rays by the ceiling's edges of MovingInterior's
		// room saw the sun from behind the wall, white speckles a few voxels apart along the edge that never settled, the
		// user's report, 2026-11-21)
		float3 lit = GiHitLitAt<kTests>(gi, point, origin + direction * hit.t, hit, origin, float(seed & 0xFFFFFFu) / 16777216.0, emission, false, true, !gap);
		// (what it met has no voxel to stand for it, kGiHitUnknown: nothing known of the pixel by this ray)
		// (the gather's rays keep count of them: no light, and none known, so that what the filter and the frames make of
		// the pixel's rays is their light with the share of them that is known beside it, which the shading divides by, and
		// where few are known takes the volume's lookup instead, see Shade. left out, as the near field's, what a thing
		// thinner than the voxels reflects onto itself went missing without a trace: the slats of the bathroom's blind,
		// whose rays mostly meet the next slat, were lit by the few that pass between them into the room alone)
		if (!GiHitKnown(lit))
			return gather ? float4(0.0, 0.0, 0.0, 0.0) : kGiNearUnknown;
		return float4(max(lit - emission, 0.0), clear);
	}
	if (gi.nearShow == 3u)
		return float4(0.0, 0.0, 0.0, 1.0);
	if (hit.capped)
		return float4(0.0, 0.0, 0.0, 0.0); // in something its traversal couldn't get through
	if (gather)
		return float4(EnvironmentRadiance(gEnvironment[0], direction, 0.6), 1.0);
	// nothing near: the light the volume holds along the ray where it ends (the irradiance over pi of a surface facing
	// back along it, see GiEvaluate), the environment's outside every clip
	GiSample far = GiSampleAt(origin + direction * reach, direction, float3(0.0), GiSeenNone());
	if (far.valid)
		return float4(GiEvaluate(far, direction), 1.0);
	if (far.inside)
		return kGiNearUnknown;
	return float4(EnvironmentIrradiance(gEnvironment[0], direction), 1.0);
}

// the surface cache's blocks the frame's pixels' lookups will read, stamped before the indirect lighting's resolve (GI
// Seen, at every pixel of the pre-pass's depth: GiMarkSeen), so that the filter runs in a block the frame it is first
// read. the stamps were the main pass's alone, of the frame before: a block read for the first time (the finest clip's
// voxels the camera walks into, what a turn brings into view) showed its entries unfiltered for a frame, each voxel
// its own few samples' mean. walking towards MovingInterior's corner, the wall ahead was specked with bright voxels for
// a frame as the finest clip's lookups reached it (the user's report, 2026-11-18: "unwanted bright artifacts are
// popping up sharply during a few frames"). what the late pre-pass draws (what the frame before hid) is the main pass's
// to stamp still
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeGiSeen(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 pixel = int2(dispatchThreadId.xy);
	ViewData view;
	ShadowSurface surface;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, surface))
		return;
	GiMarkSeen(surface.position, surface.normal, normalize(view.eyePosition.xyz - surface.position), true);
}

// the rays of every stride-th pixel each way (GiData::nearStride: a thread each), a quarter of the pixels at 2, a
// sixteenth at 4, which the filter gathers over its reach (Bistro's street took 3.4 ms with a ray a pixel)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeGiNear(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D raw = gRWTextures[SHADER_TYPES_GI_NEAR_RAW_RW_TEXTURE];
	uint2 extent;
	raw.GetDimensions(extent.x, extent.y);
	GiData gi = GiClip(0);
	int2 pixel = int2(dispatchThreadId.xy) * int(max(gi.nearStride, 1u)) + GiNearOffset(gi);
	if (any(pixel >= int2(extent)))
		return;
	raw[pixel] = GiNearRay(pixel);
}

// the lookups' sides at every pixel of the pre-pass's depth (see GiSidesMask), for the main pass's opaque fragments'
// lookups (GiSidesHidden): rays only where a voxel around the pixel's point is marked apart, and only for the masks the
// last frame doesn't hold. a mask is a sight ray to each of a lookup's voxels, of geometry that doesn't move, and made at
// every pixel every frame they were 1.7 to 7.2 million rays a frame in the glTF research set's scenes at 2560x1440
// balanced, 7 to 62 times the probes' (a thread group paid for about a third more: little to gain by keeping its
// threads busy). so a pixel takes the masks its point had the last frame (SHADER_TYPES_GI_SIDES_HISTORY_TEXTURE: found
// through the view's last view projection, on the same surface by its distance and normal, of the same voxels by
// GiSidesKey), which most have: the bathroom's 7.2 million rays a frame came to 0.5 million, the pass's 6.7 ms to 2.2,
// classroom's 8.0 to 1.9. see the tiles' turns below for what keeps them fresh
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeGiSides(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	GiData gi = GiClip(0);
	uint current = gi.sidesFrame & 1u;
	RWTexture2D sides = gRWTextures[SHADER_TYPES_GI_SIDES_RW_TEXTURE];
	RWTexture2D history = gRWTextures[SHADER_TYPES_GI_SIDES_HISTORY_RW_TEXTURE + current];
	uint2 extent;
	sides.GetDimensions(extent.x, extent.y);
	int2 pixel = int2(dispatchThreadId.xy);
	float4 kept = float4(0.0, 0.0, -1.0, 0.0);
	if (all(pixel < int2(extent)))
	{
		ViewData view;
		ShadowSurface surface;
		if (ViewAt(pixel, view) && ShadowSurfaceAt(pixel, view, surface))
		{
			uint clips[2];
			GiSidesClips(surface.position, surface.normal, clips);
			uint masks[2] = {0u, 0u};
			uint keys[2] = {0u, 0u};
			bool found[2] = {clips[0] == ~0u, clips[1] == ~0u};
			for (uint keyIt = 0; keyIt < 2u; keyIt++)
			{
				if (found[keyIt])
					continue;
				GiClipView clip = GiClipViewAt(clips[keyIt]);
				keys[keyIt] = GiSidesKey(clip, int3(floor(GiSidesTap(clip, surface.position, surface.normal))));
			}
			float3 toEye = view.eyePosition.xyz - surface.position;
			// (a tile of 8 by 8 pixels makes its masks again every sidesKeepFrameCount-th frame whatever it holds, a
			// diagonal of tiles a frame: what a mask goes by changes as the cache settles, the voxels' marks and where
			// their light is gathered, and a thread group traces as one)
			uint2 tile = uint2(pixel) >> 3u;
			bool keep = gi.sidesKeepFrameCount != 0u && (tile.x + tile.y * 3u + gi.sidesFrame) % gi.sidesKeepFrameCount != 0u;
			float4 was = mul(view.previousViewProjection, float4(surface.position, 1.0));
			if (keep && view.previousEyePosition.w > 0.0 && was.w > 0.0)
			{
				// (the last frame's pixels around where the point was then, the nearest first: one whose surface is the
				// point's, as the near field's history tells, ComputeGiNearTemporal, holds its masks, and where the point
				// is by a voxel's face the pixel beside it does)
				float2 at = PixelHistoryAt(view, was);
				float expected = distance(surface.position, view.previousEyePosition.xyz);
				float slant = abs(dot(surface.normal, normalize(toEye)));
				float tolerance = 0.002 * expected + 2.0 * surface.metersPerPixel * sqrt(max(1.0 - slant * slant, 0.0)) / max(slant, 0.05);
				Texture2D last = gTextures[SHADER_TYPES_GI_SIDES_HISTORY_TEXTURE + (current ^ 1u)];
				int2 nearest = int2(floor(at + 0.5));
				static const int2 kTaps[9] = {int2(0, 0), int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)};
				for (uint tapIt = 0; tapIt < 9u && !(found[0] && found[1]); tapIt++)
				{
					int2 tap = nearest + kTaps[tapIt];
					if (any(float2(tap) < view.viewport.xy) || any(float2(tap) >= view.viewport.xy + view.viewport.zw))
						continue;
					float4 then = last.Load(int3(tap, 0));
					if (then.b < 0.0 || abs(then.b - expected) > tolerance || dot(EmitterUnpackNormal(then.a), surface.normal) < 0.9)
						continue;
					uint old[2] = {asuint(then.r), asuint(then.g)};
					for (uint slotIt = 0; slotIt < 2u; slotIt++)
						for (uint oldIt = 0; oldIt < 2u; oldIt++)
							if (!found[slotIt] && (old[oldIt] & kGiSidesKeyMask) == keys[slotIt])
							{
								masks[slotIt] = old[oldIt];
								found[slotIt] = true;
							}
				}
			}
			for (uint slotIt = 0; slotIt < 2u; slotIt++)
			{
				if (found[slotIt])
					continue;
				masks[slotIt] = (GiSidesMaskFacing(GiClipViewAt(clips[slotIt]), surface.position, surface.normal) & ~(0x3Fu << kGiSidesKeyShift)) | keys[slotIt];
			}
			kept = float4(asfloat(masks[0]), asfloat(masks[1]), length(toEye), EmitterPackNormal(surface.normal));
		}
		sides[pixel] = float4(kept.rg, 0.0, 0.0);
		history[pixel] = kept;
	}
}

// a pass of the near field's filter: the rays' radiance and whether they met no contact (or the rows' result) averaged
// along the row (or column) over the traced pixels within 16, by a tent of their distance (the same kernel whatever the
// pixel's parity: a box of the 17 traced pixels around a traced one and the 16 around the others made neighbors' sums
// differ, stripes a pixel wide, fixed to the screen, and drew a bright ray as a square), weighed by how much each is on
// the pixel's surface (its distance off the pixel's tangent plane, against a pixel's width, and how its normal agrees, as the
// traced shadows' filter), leaving out what is unknown
float4 GiNearFilter(int2 pixel, int2 step, uint source)
{
	ViewData view;
	ShadowSurface center;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, center))
		return kGiNearUnknown;
	Texture2D input = gTextures[source];
	float2 low = view.viewport.xy;
	float2 high = view.viewport.xy + view.viewport.zw;
	float4 total = 0.0;
	float weights = 0.0;
	int axis = dot(pixel, step);
	// the traced pixels along the axis within 16: every stride-th, so the first is the one at or past -16 congruent to the
	// pixel's place
	// (the frame's of each stride's square, GiNearOffset)
	GiData gi = GiClip(0);
	int stride = int(max(gi.nearStride, 1u));
	int first = -16 + ((16 - axis + dot(GiNearOffset(gi), step)) % stride + stride) % stride;
	for (int offset = first; offset <= 16; offset += stride)
	{
		int2 at = pixel + step * offset;
		if (any(float2(at) < low) || any(float2(at) >= high))
			continue;
		float4 value = input.Load(int3(at, 0));
		if (value.a < 0.0)
			continue;
		float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(at, 0)).r;
		if (depth <= 0.0)
			continue;
		float3 position = ShadowPosition(view, float2(at), depth);
		float3 normal = gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(at, 0)).xyz * 2.0 - 1.0;
		float plane = abs(dot(position - center.position, center.normal)) / (2.0 * center.metersPerPixel);
		float facing = saturate(dot(normal, center.normal) * rsqrt(max(dot(normal, normal), 1e-8)));
		float w = exp(-plane) * pow(facing, 8.0) * (17.0 - abs(float(offset)));
		total += w * value;
		weights += w;
	}
	return weights > 0.0 ? total / weights : kGiNearUnknown;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeGiNearX(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 extent;
	gRWTextures[SHADER_TYPES_GI_NEAR_ROWS_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	// the traced pixels only (a thread each): the columns' pass, the frames' and the main pass's upsampling read nothing
	// else. at every pixel, as they were, the filter and the frames cost 5 to 9 ms of Bistro's and San Miguel's frames
	GiData gi = GiClip(0);
	int2 pixel = int2(dispatchThreadId.xy) * int(max(gi.nearStride, 1u)) + GiNearOffset(gi);
	if (any(pixel >= int2(extent)))
		return;
	gRWTextures[SHADER_TYPES_GI_NEAR_ROWS_RW_TEXTURE][pixel] = GiNearFilter(pixel, int2(1, 0), SHADER_TYPES_GI_NEAR_RAW_TEXTURE);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeGiNearY(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 extent;
	gRWTextures[SHADER_TYPES_GI_NEAR_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	GiData gi = GiClip(0);
	int2 pixel = int2(dispatchThreadId.xy) * int(max(gi.nearStride, 1u)) + GiNearOffset(gi);
	if (any(pixel >= int2(extent)))
		return;
	gRWTextures[SHADER_TYPES_GI_NEAR_RW_TEXTURE][pixel] = GiNearFilter(pixel, int2(0, 1), SHADER_TYPES_GI_NEAR_ROWS_TEXTURE);
}

// the frame's filtered near field at a pixel (its surface, the pre-pass's), from the traced pixels around (their
// surfaces from the pre-pass's depth and normals there): bilinear over the 2x2 of them around, each by how much it is on
// the pixel's surface (its distance off the pixel's tangent plane and its normal's agreement, as EmitterUpsample), else
// the 3x3 by that alone, else (thin geometry between the traced pixels) by how their normals agree; kGiNearUnknown where
// none is known. first: the traced pixel of each stride's square (GiNearOffset). own: how much of it is the pixel's own
// surface's (the bilinear weights of the traced pixels that are on it: 1 at a traced pixel, 0 where it took the others)
float4 GiNearUpsample(int2 pixel, ShadowSurface surface, ViewData view, int stride, int2 first, out float own)
{
	Texture2D light = gTextures[SHADER_TYPES_GI_NEAR_TEXTURE];
	float2 low = view.viewport.xy;
	float2 high = view.viewport.xy + view.viewport.zw;
	float scale = max(0.01 * distance(surface.position, view.eyePosition.xyz), 1e-6);
	float2 at = float2(pixel - first) / float(stride);
	int2 base = int2(floor(at));
	float2 f = at - float2(base);
	float4 total = 0.0;
	float weights = 0.0;
	for (int corner = 0; corner < 4; corner++)
	{
		int2 offset = int2(corner & 1, corner >> 1);
		int2 tap = (base + offset) * stride + first;
		if (any(float2(tap) < low) || any(float2(tap) >= high))
			continue;
		float4 value = light.Load(int3(tap, 0));
		float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(tap, 0)).r;
		if (value.a < 0.0 || depth <= 0.0)
			continue;
		float3 position = ShadowPosition(view, float2(tap), depth);
		float3 normal = normalize(gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(tap, 0)).xyz * 2.0 - 1.0);
		float similar = exp(-abs(dot(position - surface.position, surface.normal)) / scale) * pow(saturate(dot(normal, surface.normal)), 8.0);
		float w = (offset.x != 0 ? f.x : 1.0 - f.x) * (offset.y != 0 ? f.y : 1.0 - f.y) * similar;
		total += w * value;
		weights += w;
	}
	own = saturate(weights);
	if (weights < 1e-4)
	{
		total = 0.0;
		weights = 0.0;
		float4 alikeTotal = 0.0;
		float alikeWeights = 0.0;
		int2 nearest = int2(round(at));
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++)
			{
				int2 tap = (nearest + int2(x, y)) * stride + first;
				if (any(float2(tap) < low) || any(float2(tap) >= high))
					continue;
				float4 value = light.Load(int3(tap, 0));
				float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(tap, 0)).r;
				if (value.a < 0.0 || depth <= 0.0)
					continue;
				float3 position = ShadowPosition(view, float2(tap), depth);
				float3 normal = normalize(gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(tap, 0)).xyz * 2.0 - 1.0);
				float facing = saturate(dot(normal, surface.normal));
				float w = exp(-abs(dot(position - surface.position, surface.normal)) / scale) * pow(facing, 8.0);
				total += w * value;
				weights += w;
				alikeTotal += (facing + 0.01) * value;
				alikeWeights += facing + 0.01;
			}
		if (weights <= 1e-6)
		{
			total = alikeTotal;
			weights = alikeWeights;
		}
	}
	return weights > 0.0 ? total / weights : kGiNearUnknown;
}

// a pixel's near field history as PixelHistory's (see there), but of the pixels whose surfaces' fourth channel was mark
// alone (1 where the surface moves, so that what moves takes no history from what it passed in front of, nor the other way)
float4 GiNearHistory(Texture2D last, Texture2D lastSurfaces, ViewData view, float2 at, float expected, float tolerance, float3 normal, float mark, out float weights, out float held)
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
		float4 seen = lastSurfaces.Load(int3(tap, 0));
		if (value.a < 0.0 || seen.x < 0.0 || abs(seen.x - expected) > tolerance || dot(EmitterUnpackNormal(seen.y), normal) < 0.9 || seen.w != mark)
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
			float4 seen = lastSurfaces.Load(int3(tap, 0));
			if (value.a < 0.0 || seen.x < 0.0 || abs(seen.x - expected) > tolerance || dot(EmitterUnpackNormal(seen.y), normal) < 0.9 || seen.w != mark)
				continue;
			float2 d = float2(tap) - at;
			float w = 1.0 / (1.0 + dot(d, d));
			total += w * value;
			weights += w;
			held += w * seen.z;
		}
	return total;
}

// the near field over frames (GI Near Temporal; GiData::nearTemporal), per pixel of the pre-pass's depth: the frame's
// filtered rays upsampled from the traced pixels around (GiNearUpsample), blended into what the last frame's history
// holds where the pixel's point was then (by the view's last view projection, ViewData::previousViewProjection, the
// jitters taken out: see PixelHistoryAt), by GiData::nearBlend (1 where there is no history, or the frames are off: the
// frame's alone). the history is bilinear over the 4 pixels around that place, each only if its point was as far from
// the last frame's eye as this pixel's is now (within a fifth of a percent and the depth two pixels make along the
// surface: else it saw something else there, revealed or moved) and its normal within 25 degrees, else the 3x3 around
// that are (see PixelHistory); what moves (the main depth's stencil) by its own motion (the pre-pass's), from the pixels
// of what moved alone and the rest from the rest's alone (the surfaces' mark), so neither ghosts. the frame's
// history and its pixels' surfaces (distance and normal, as the emitters', and whether it moves) go to the images the frame's parity says
// (SHADER_TYPES_GI_NEAR_HISTORY_TEXTURE). per pixel rather than per traced pixel, as the
// emitters' (see ComputeEmitterTemporal): under temporal upscaling's jitter, the pixels upsampled from a traced pixel at
// an edge changed as a block, which the upscaler passed on (the near field's light alone, the indirect diffuse debug view
// with SPEEDO_GI_NEAR_SHOW=1, at the user's San Miguel pose 7.6715,4.3834,1.7963,-22.087,-88.462 at performance, a
// stride of 4 and resolution 64: 870 pixels a frame changed by more than 32 of 255, 447 per pixel; by more than 64, 200
// and 41)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeGiNearTemporal(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	GiData gi = GiClip(0);
	uint current = gi.nearFrame & 1u;
	RWTexture2D history = gRWTextures[SHADER_TYPES_GI_NEAR_HISTORY_RW_TEXTURE + current];
	RWTexture2D surfaces = gRWTextures[SHADER_TYPES_GI_NEAR_HISTORY_DISTANCE_RW_TEXTURE + current];
	uint2 extent;
	history.GetDimensions(extent.x, extent.y);
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(pixel >= int2(extent)))
		return;
	ViewData view;
	ShadowSurface surface;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, surface))
	{
		history[pixel] = kGiNearUnknown;
		surfaces[pixel] = float4(-1.0, 0.0, 0.0, 0.0);
		return;
	}
	float3 toEye = view.eyePosition.xyz - surface.position;
	float own;
	float4 now = GiNearUpsample(pixel, surface, view, int(max(gi.nearStride, 1u)), GiNearOffset(gi), own);
	// the frames the history holds (the gather's: as much as this one is of the pixel's own surface, see below)
	float count = gi.nearGather != 0u ? max(own, 0.02) : 1.0;
	float4 result = now;
	bool moves = (gMainStencil.Load(int3(pixel, 0)) & 1u) != 0u;
	float4 clip = mul(view.previousViewProjection, float4(surface.position, 1.0));
	if (now.a >= 0.0 && gi.nearBlend < 1.0 && view.previousEyePosition.w > 0.0 && clip.w > 0.0)
	{
		float2 at = PixelHistoryAt(view, clip);
		float expected = distance(surface.position, view.previousEyePosition.xyz);
		float slant = abs(dot(surface.normal, normalize(toEye)));
		float tolerance = 0.002 * expected + 2.0 * surface.metersPerPixel * sqrt(max(1.0 - slant * slant, 0.0)) / max(slant, 0.05);
		// what moves by where its point was (the pre-pass's motion, as temporal upscaling reprojects it), from the pixels
		// of what moved last frame alone (their surfaces' mark, below), within half its distance (where it was isn't
		// known, only where it showed: MovingInterior's sphere comes 7.5 cm nearer a frame at 60 frames a second, a
		// quarter of its distance 30 cm from the lens; within a tenth, it lost its history under 75 cm, grainy patches as
		// it passed the camera).
		// it took none, and its near field was the frame's rays alone, a sixteenth of the pixels: a blotchy light, which
		// the shading let through wherever its lookup saw its probes a little less (a tenth of it at a trust of 0.9),
		// and the sphere flickered dark in patches whenever it crossed a row of probes (the user's report, 2026-11-18)
		if (moves)
		{
			at = float2(pixel) + gTextures[SHADER_TYPES_PREPASS_MOTION_TEXTURE].Load(int3(pixel, 0)).xy;
			tolerance += 0.5 * expected;
		}
		Texture2D last = gTextures[SHADER_TYPES_GI_NEAR_HISTORY_TEXTURE + (current ^ 1u)];
		Texture2D lastSurfaces = gTextures[SHADER_TYPES_GI_NEAR_HISTORY_DISTANCE_TEXTURE + (current ^ 1u)];
		float weights;
		float held;
		float4 total = GiNearHistory(last, lastSurfaces, view, at, expected, tolerance, surface.normal, moves ? 1.0 : 0.0, weights, held);
		// the frame blended in by 1 over the frames the history holds, at least the blend, or while the view is still and
		// nothing may change its light the still blend (GiData::nearStillBlend), as the emitters' light (see
		// ComputeEmitterTemporal): a mean of the last 16 frames' rays never settles, and what was left of their noise
		// wandered at the pace the frames replaced it, the light in a lit window's opening never still (the user's pose over
		// Bistro's facades, 724.7479,659.6074,-2775.4233,-21.034,93.844 at 02:14)
		// and a frame far brighter than what the history holds taken as at most a few times that (HistoryFireflyClamp, as
		// the emitters' light): a traced pixel's ray of the frame that meets what is bright, the voxels by a lit window's
		// interior from a window frame's side across the opening, is a sixteenth of what the pixels around take (at a
		// stride of 4), and faded over the history's 16 frames: white fireflies along the window frames at night (the
		// same pose)
		if (weights > 1e-3)
		{
			float4 kept = total / weights;
			// (what moves never by the still blend: its light changes as it moves)
			float least = EmitterViewStill(view) && !moves ? gi.nearStillBlend : gi.nearBlend;
			// (the gather's frames count by how much of the frame is the pixel's own surface's, GiNearUpsample's own: its rays
			// move over the pixels, GiNearOffset, so a pixel between surfaces, whose neighbours' light is another surface's,
			// keeps what the frames with rays of its own gave it)
			float sample = gi.nearGather != 0u ? max(own, 0.02) : 1.0;
			count = min(max(held / weights, gi.nearGather != 0u ? 0.02 : 1.0) + sample, 1.0 / least);
			result = lerp(kept, float4(HistoryFireflyClamp(now.rgb, kept.rgb, view.exposure), now.a), max(sample / count, least * sample));
		}
	}
	history[pixel] = result;
	surfaces[pixel] = float4(length(toEye), EmitterPackNormal(surface.normal), count, moves ? 1.0 : 0.0);
}

// what the near field's rays gathered at the main pass's opaque fragment's pixel (over frames, its history there, whose
// surface the fragment is: see ComputeGiNearTemporal), kGiNearUnknown where it has none (other fragments, or no ray)
float4 GiNearAtFragment()
{
	if (!gFragmentGiNear)
		return kGiNearUnknown;
	return gTextures[SHADER_TYPES_GI_NEAR_HISTORY_TEXTURE + (GiClip(0).nearFrame & 1u)].Load(int3(gFragmentPixel, 0));
}

// the weight of a volume's lookup (GiSample::last.w, how much its voxels see of the probes around them) from which it is
// taken whole where the near field's light stands in for it (GiLookupTrust): where the lookups see their probes it is a
// half or more (92 to 97% of the pixels at Sponza's, Bistro's and salle_de_bain's poses), and where they barely do under a
// tenth; between, almost none
static const float kGiNearTrustedWeight = 0.2;

// how far a volume's lookup is to be taken where the near field's light can stand in for it: by its weight, none where it
// sees no probe. normalized by its weight, a lookup that barely sees one probe takes all of that probe's light: on the
// floor in front of Sponza's curtains, which hang a few centimetres over it (the user's pose
// -3.0027,0.2878,2.2588,9.549,338.778), the voxels beside the hem barely see the probes beyond it, in the sunlit
// courtyard, and lit the floor along it in jagged white patches, which the gold strip at the curtains' foot reflected
float GiLookupTrust(GiSample sample)
{
	return sample.valid ? saturate(sample.last.w / kGiNearTrustedWeight) : 0.0;
}

// and of something moving's surface (GiSurfaceBias's moving), from a tenth of that weight: its voxels and the air's beside
// it hold the probes' light alone, at the probes' weight (their facing's 4th power, about a tenth where they see it
// well), where a still surface's hold the cache's at its confidence, 1 once settled; a probe hidden counts a thousandth.
// by the still surfaces' line, MovingInterior's sphere was half the near field's light over its outer third (flat, the
// volume's along its rays' ends: 96 of 255 where the lookup's shading along its normal held 38 to 60), by a share that
// changed every frame with where it stood among the voxels: its rim 59, 57 and 45 in three frames at 60 a second,
// dark rings and patches flickering over it (the user's report, 2026-11-18); the lookup alone held 38, 37, 38
static const float kGiNearTrustedMovingWeight = 0.02;

float GiLookupTrust(GiSample sample, bool moving)
{
	return sample.valid ? saturate(sample.last.w / (moving ? kGiNearTrustedMovingWeight : kGiNearTrustedWeight)) : 0.0;
}

// the main pass's opaque fragments' diffuse indirect light (see Shade): the volume's lookup (indirect, -1 outside the
// clips; trust: how far it is to be taken, GiLookupTrust) where no other surface is nearer than a contact
// (GiData::nearContact), what the near field's rays gathered (near, GiNearAtFragment) where all of them are, between them
// by the power of the share of the rays that met no contact (GiData::nearPower), which is times the trust of the traced
// pixels' lookups around (see GiNearRay). the floor in front of a curtain over a gap of a few centimetres isn't at a
// contact, a tenth of its rays meeting the curtain, and kept the lookup's light; the lookup's second try
// (GiSampleSurface's retry, twice as far off the surface), where the first saw no probe, found the voxels lit from the
// courtyard there: where the near field's light is, it stands in for that instead (see Shade). by the fragment's own
// trust alone, the region where the lookups fail kept its outline, a staircase of voxels, where a lookup's weight jumps
// from 0 to a tenth
float3 GiNearIrradiance(float4 near, float3 indirect, float trust)
{
	if (near.a < 0.0 || indirect.x < 0.0)
		return indirect;
	uint show = gGi[gPushConstants.frameIndex][0].nearShow;
	if (show == 1u || show == 3u || show == 4u)
		return near.rgb;
	if (show == 2u)
		return float3(pow(saturate(near.a), 2.2));
	return lerp(near.rgb, indirect, pow(saturate(near.a), gGi[gPushConstants.frameIndex][0].nearPower) * trust);
}
