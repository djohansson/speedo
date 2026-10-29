// the indirect lighting's near field: what a pixel's surface sees within a few voxels of it, which the volume's voxels
// can't hold. where a sphere rests on a floor, the floor's voxels at the contact are also the sphere's never drawn inside,
// which gets no light (see ComputeGiResolve), and the gap beside it is thinner than a voxel, all its surfaces lit by the
// gap's own unlit voxels: the volume's lookups there find no light, in blocks a voxel wide (the user's CornellBox poses,
// black rectangles under the spheres). per pixel of the pre-pass's depth, a ray from the surface through the BVHs, as far
// as GiData::nearReach voxels of the clips that light the point (GI Near, ComputeGiNear), its direction one of 16 over
// the 4x4 tile it is in (cosine distributed): the radiance of what it meets (the voxels' there, see GiRadianceAtHit), or
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

// a pixel's ray (see ComputeGiNear): rgb its radiance, a 0 where it met something nearer than a contact
// (GiData::nearContact), else 1, -1 where there is nothing to know (no surface, outside the clips, or the volume's lookup
// at its end saw no probe)
static const float4 kGiNearUnknown = float4(0.0, 0.0, 0.0, -1.0);

float4 GiNearRay(int2 pixel)
{
	ViewData view;
	ShadowSurface surface;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, surface))
		return kGiNearUnknown;
	GiData gi = GiClip(0);
	if (gi.enabled == 0u || gi.nearReach <= 0.0 || !GiTracesBvh(gi))
		return kGiNearUnknown;
	float voxel = GiNearVoxelSize(surface.position);
	if (voxel <= 0.0)
		return kGiNearUnknown;
	float reach = gi.nearReach * voxel;
	float3 direction = GiNearDirection(pixel, surface.normal, gi.nearTemporal != 0u ? gi.nearFrame : 0u, max(gi.nearStride, 1u));
	// off the surface by a pixel and a half (more far from the eye), as the traced shadows' rays, against the depth's
	// and the BVH's rounding
	float bias = max(1.5 * surface.metersPerPixel, 1e-4 * distance(surface.position, view.eyePosition.xyz));
	float3 origin = surface.position + surface.normal * bias;
	BvhHit hit = GiTraceTriangles(gi, origin, direction, reach, false, true, false);
	if (BvhHitSomething(hit))
	{
		float clear = hit.t < gi.nearContact * voxel ? 0.0 : 1.0;
		// a single sided surface seen from behind: the ray is inside what it bounds, where no light is
		if (!hit.frontFace && (hit.flags & BVH_TRIANGLE_FLAG_DOUBLE_SIDED) == 0u)
			return float4(0.0, 0.0, 0.0, clear);
		if (gi.nearShow == 4u)
			return float4(0.0, 0.0, 0.0, clear);
		// the radiance of what it met, the voxels' there (see GiRadianceAtHit). within a voxel and a half of the pixel's
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
		float needed = max(1.5 * voxel - dot(point - surface.position, surface.normal), 0.0);
		if (needed > 0.0)
		{
			float3 along = surface.normal - hit.normal * dot(surface.normal, hit.normal);
			float alongLength = length(along);
			float move = alongLength > 1e-3 ? needed / alongLength : 3.0 * voxel + 1.0;
			if (move > 3.0 * voxel)
			{
				if (dot(hit.normal, surface.normal) < 0.0)
					return float4(0.0, 0.0, 0.0, clear);
				move = 3.0 * voxel;
			}
			if (alongLength > 1e-3)
				point += along * (move / alongLength);
		}
		// the light it reflects alone (GiHitReflected): an emitter's is the volume's, which averages it over its probes'
		// rays and frames. a small one met by one ray in four pixels is a firefly: Bistro's lantern bulbs, a few
		// centimetres of 3.5, speckled the walls around them white, where they give a hundredth of the daylight
		return float4(GiHitReflected(0u, point, hit.normal), clear);
	}
	if (gi.nearShow == 3u)
		return float4(0.0, 0.0, 0.0, 1.0);
	if (hit.capped)
		return float4(0.0, 0.0, 0.0, 0.0); // in something its traversal couldn't get through
	// nothing near: the light the volume holds along the ray where it ends (the irradiance over pi of a surface facing
	// back along it, see GiEvaluate), the environment's outside every clip
	GiSample far = GiSampleAt(origin + direction * reach, direction, float3(0.0));
	if (far.valid)
		return float4(GiEvaluate(far, direction), 1.0);
	if (far.inside)
		return kGiNearUnknown;
	return float4(EnvironmentIrradiance(gEnvironment[0], direction), 1.0);
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
	int2 pixel = int2(dispatchThreadId.xy) * int(max(GiClip(0).nearStride, 1u));
	if (any(pixel >= int2(extent)))
		return;
	raw[pixel] = GiNearRay(pixel);
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
	int stride = int(max(GiClip(0).nearStride, 1u));
	int first = -16 + ((16 - axis) % stride + stride) % stride;
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
	int2 pixel = int2(dispatchThreadId.xy) * int(max(GiClip(0).nearStride, 1u));
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
	int2 pixel = int2(dispatchThreadId.xy) * int(max(GiClip(0).nearStride, 1u));
	if (any(pixel >= int2(extent)))
		return;
	gRWTextures[SHADER_TYPES_GI_NEAR_RW_TEXTURE][pixel] = GiNearFilter(pixel, int2(0, 1), SHADER_TYPES_GI_NEAR_ROWS_TEXTURE);
}

// the frame's filtered near field at a pixel (its surface, the pre-pass's), from the traced pixels around (their
// surfaces from the pre-pass's depth and normals there): bilinear over the 2x2 of them around, each by how much it is on
// the pixel's surface (its distance off the pixel's tangent plane and its normal's agreement, as EmitterUpsample), else
// the 3x3 by that alone, else (thin geometry between the traced pixels) by how their normals agree; kGiNearUnknown where
// none is known
float4 GiNearUpsample(int2 pixel, ShadowSurface surface, ViewData view, int stride)
{
	Texture2D light = gTextures[SHADER_TYPES_GI_NEAR_TEXTURE];
	float2 low = view.viewport.xy;
	float2 high = view.viewport.xy + view.viewport.zw;
	float scale = max(0.01 * distance(surface.position, view.eyePosition.xyz), 1e-6);
	float2 at = float2(pixel) / float(stride);
	int2 base = int2(floor(at));
	float2 f = at - float2(base);
	float4 total = 0.0;
	float weights = 0.0;
	for (int corner = 0; corner < 4; corner++)
	{
		int2 offset = int2(corner & 1, corner >> 1);
		int2 tap = (base + offset) * stride;
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
				int2 tap = (nearest + int2(x, y)) * stride;
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

// the near field over frames (GI Near Temporal; GiData::nearTemporal), per pixel of the pre-pass's depth: the frame's
// filtered rays upsampled from the traced pixels around (GiNearUpsample), blended into what the last frame's history
// holds where the pixel's point was then (by the view's last view projection, ViewData::previousViewProjection, the
// jitters taken out: see PixelHistoryAt), by GiData::nearBlend (1 where there is no history, or the frames are off: the
// frame's alone). the history is bilinear over the 4 pixels around that place, each only if its point was as far from
// the last frame's eye as this pixel's is now (within a fifth of a percent and the depth two pixels make along the
// surface: else it saw something else there, revealed or moved) and its normal within 25 degrees, else the 3x3 around
// that are (see PixelHistory), and what moves (the main depth's stencil) takes none, so neither ghosts. the frame's
// history and its pixels' surfaces (distance and normal, as the emitters') go to the images the frame's parity says
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
	surfaces[pixel] = float4(length(toEye), EmitterPackNormal(surface.normal), 0.0, 0.0);
	float4 now = GiNearUpsample(pixel, surface, view, int(max(gi.nearStride, 1u)));
	float4 result = now;
	bool moves = (gMainStencil.Load(int3(pixel, 0)) & 1u) != 0u;
	float4 clip = mul(view.previousViewProjection, float4(surface.position, 1.0));
	if (now.a >= 0.0 && gi.nearBlend < 1.0 && view.previousEyePosition.w > 0.0 && !moves && clip.w > 0.0)
	{
		float2 at = PixelHistoryAt(view, clip);
		float expected = distance(surface.position, view.previousEyePosition.xyz);
		float slant = abs(dot(surface.normal, normalize(toEye)));
		float tolerance = 0.002 * expected + 2.0 * surface.metersPerPixel * sqrt(max(1.0 - slant * slant, 0.0)) / max(slant, 0.05);
		Texture2D last = gTextures[SHADER_TYPES_GI_NEAR_HISTORY_TEXTURE + (current ^ 1u)];
		Texture2D lastSurfaces = gTextures[SHADER_TYPES_GI_NEAR_HISTORY_DISTANCE_TEXTURE + (current ^ 1u)];
		float weights;
		float4 total = PixelHistory(last, lastSurfaces, view, at, expected, tolerance, surface.normal, weights);
		// and a frame far brighter than what the history holds taken as at most a few times that (HistoryFireflyClamp, as
		// the emitters' light): a traced pixel's ray of the frame that meets what is bright, the voxels by a lit window's
		// interior from a window frame's side across the opening, is a sixteenth of what the pixels around take (at a
		// stride of 4), and faded over the history's 16 frames: white fireflies along the window frames at night (the
		// user's pose over Bistro's facades, 724.7479,659.6074,-2775.4233,-21.034,93.844 at 02:14)
		if (weights > 1e-3)
		{
			float4 kept = total / weights;
			result = lerp(kept, float4(HistoryFireflyClamp(now.rgb, kept.rgb), now.a), gi.nearBlend);
		}
	}
	history[pixel] = result;
}

// the main pass's opaque fragments' diffuse indirect light (see Shade): the volume's lookup (indirect, -1 outside the
// clips; valid: whether it saw a probe) where no other surface is nearer than a contact (GiData::nearContact), what the
// near field's rays gathered where all of them are (over frames, its history at the fragment's pixel, whose surface the
// fragment is: see ComputeGiNearTemporal), between them by the power of the share of the rays that met no contact
// (GiData::nearPower)
float3 GiNearIrradiance(float3 indirect, bool valid)
{
	float4 near = gTextures[SHADER_TYPES_GI_NEAR_HISTORY_TEXTURE + (GiClip(0).nearFrame & 1u)].Load(int3(gFragmentPixel, 0));
	if (near.a < 0.0 || indirect.x < 0.0)
		return indirect;
	uint show = gGi[gPushConstants.frameIndex][0].nearShow;
	if (show == 1u || show == 3u || show == 4u)
		return near.rgb;
	if (show == 2u)
		return float3(pow(saturate(near.a), 2.2));
	if (!valid)
		return near.rgb;
	return lerp(near.rgb, indirect, pow(saturate(near.a), gGi[gPushConstants.frameIndex][0].nearPower));
}
