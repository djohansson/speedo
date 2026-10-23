// the indirect lighting's near field: what a pixel's surface sees within a few voxels of it, which the volume's voxels
// can't hold. where a sphere rests on a floor, the floor's voxels at the contact are also the sphere's never drawn inside,
// which gets no light (see ComputeGiResolve), and the gap beside it is thinner than a voxel, all its surfaces lit by the
// gap's own unlit voxels: the volume's lookups there find no light, in blocks a voxel wide (the user's CornellBox poses,
// black rectangles under the spheres). per pixel of the pre-pass's depth, a ray from the surface through the BVHs, as far
// as GiData::nearReach voxels of the clips that light the point (GI Near, ComputeGiNear), its direction one of 16 over
// the 4x4 tile it is in (cosine distributed): the radiance of what it meets (the voxels' there, see GiRadianceAtHit), or
// where it meets nothing, the volume's light along it from its end; filtered over the pixels around on the same surface
// (GI Near X and Y, ComputeGiNearX and Y): the light the pixel gathers, and the share of its rays that met nothing near,
// which the main pass's opaque fragments take in place of the volume's lookup by how much is near (GiNearIrradiance). a
// part of shaders.slang, which includes it after tracedshadows.slang

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

// a traced pixel's ray (every other pixel each way, see ComputeGiNear): one of 16 directions over the 4x4 tile of traced
// pixels it is in, cosine distributed about the normal (a point of the unit disk lifted onto the hemisphere, Malley's
// method): the disk's points as the traced shadows' (ShadowDiskPoint, every row and column of a tile all four bands of
// the radius and quarters of the circle), turned per tile by a hash of its own
float3 GiNearDirection(int2 pixel, float3 normal)
{
	float2 disk = ShadowDiskPoint(pixel >> 1, 0x9e3779b9u);
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
	float3 direction = GiNearDirection(pixel, surface.normal);
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
		// along the hit's surface (the pixel's normal in its tangent plane)
		float3 point = origin + direction * hit.t;
		float above = dot(point - surface.position, surface.normal);
		float3 along = surface.normal - hit.normal * dot(surface.normal, hit.normal);
		float alongLength = length(along);
		if (alongLength > 1e-3)
			point += along * (max(1.5 * voxel - above, 0.0) / alongLength);
		float3 emission;
		return float4(GiRadianceAtHit(0u, point, hit.normal, emission), clear);
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

// the rays of every other pixel each way (a thread each, the image's even rows and columns): a quarter of the pixels,
// which the filter gathers over twice the reach (Bistro's street took 3.4 ms with a ray a pixel)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeGiNear(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D raw = gRWTextures[SHADER_TYPES_GI_NEAR_RAW_RW_TEXTURE];
	uint2 extent;
	raw.GetDimensions(extent.x, extent.y);
	int2 pixel = int2(dispatchThreadId.xy) * 2;
	if (any(pixel >= int2(extent)))
		return;
	raw[pixel] = GiNearRay(pixel);
}

// a pass of the near field's filter: the rays' radiance and whether they met no contact (or the rows' result) averaged
// along the row (or column) over the traced pixels within 16 (16 or 17 of them, four tiles of the rays' directions: the
// ends at half weight, so that each of a tile's columns counts as much), weighed by how much each is on the pixel's
// surface (its distance off the pixel's tangent plane, against a pixel's width, and how its normal agrees, as the
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
	for (int offset = (axis & 1) == 0 ? -16 : -15; offset <= 16; offset += 2)
	{
		int2 at = pixel + step * offset;
		if (any(float2(at) < low) || any(float2(at) >= high))
			continue;
		float4 value = input.Load(int3(at, 0));
		if (value.a < 0.0)
			continue;
		float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(at, 0)).r;
		if (depth >= 1.0)
			continue;
		float3 position = ShadowPosition(view, float2(at), depth);
		float3 normal = gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(at, 0)).xyz * 2.0 - 1.0;
		float plane = abs(dot(position - center.position, center.normal)) / (2.0 * center.metersPerPixel);
		float facing = saturate(dot(normal, center.normal) * rsqrt(max(dot(normal, normal), 1e-8)));
		float w = exp(-plane) * pow(facing, 8.0) * (abs(offset) == 16 ? 0.5 : 1.0);
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
	// the traced rows only: the columns' pass reads nothing else
	if (any(dispatchThreadId.xy >= extent) || (dispatchThreadId.y & 1u) != 0u)
		return;
	int2 pixel = int2(dispatchThreadId.xy);
	gRWTextures[SHADER_TYPES_GI_NEAR_ROWS_RW_TEXTURE][pixel] = GiNearFilter(pixel, int2(1, 0), SHADER_TYPES_GI_NEAR_RAW_TEXTURE);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeGiNearY(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 extent;
	gRWTextures[SHADER_TYPES_GI_NEAR_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	if (any(dispatchThreadId.xy >= extent))
		return;
	int2 pixel = int2(dispatchThreadId.xy);
	gRWTextures[SHADER_TYPES_GI_NEAR_RW_TEXTURE][pixel] = GiNearFilter(pixel, int2(0, 1), SHADER_TYPES_GI_NEAR_ROWS_TEXTURE);
}

// the main pass's opaque fragments' diffuse indirect light (see Shade): the volume's lookup (indirect, -1 outside the
// clips; valid: whether it saw a probe) where no other surface is nearer than a contact (GiData::nearContact), what the
// near field's rays gathered where all of them are, between them by the power of the share of the rays that met no
// contact (GiData::nearPower)
float3 GiNearIrradiance(float3 indirect, bool valid)
{
	float4 near = gTextures[SHADER_TYPES_GI_NEAR_TEXTURE].Load(int3(int2(gFragmentPixel), 0));
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
