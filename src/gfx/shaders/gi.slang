// indirect lighting: world space radiance cascades over a voxel radiance volume (see GiData). a part of shaders.slang,
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
// rather than a copy of it all (its cascades' tables), which cost the main pass and the voxels' lighting
struct GiClipView
{
	float4 volumeOrigin;
	uint4 volumeSize;
	uint voxelFirst;
	uint clip;
	uint clipCount;
	float probeSpacing; // cascade 0's, in voxels
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
	view.probeSpacing = gGi[gPushConstants.frameIndex][clip].cascadeInterval[0].w / view.volumeOrigin.w;
	view.fadeCenter = gGi[gPushConstants.frameIndex][clip].fadeCenter;
	view.fadeExtent = gGi[gPushConstants.frameIndex][clip].fadeExtent;
	return view;
}

// how much a clip lights a point, where the finer ones don't (see GiSampleAt): the last clip all of what it holds; one that
// follows the eye 1 in the box around the eye it always holds (GiData::fadeCenter), fading to 0 toward the box's edges, and
// at most as much as it holds the point (more than a cascade 0 probe spacing inside it, fading over 8 voxels: where it is
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

// along a ray through a level of the volume, from start to end (world space distances): the radiance reaching its
// origin (front to back) and how much of what is beyond gets through. where it leaves the clip it goes on through the
// coarser ones (never back: they are boxes), at their level of the same voxel size (or level 0), at half their voxels'
// steps. a clip that follows the eye is left where its box around the eye is (GiData::fadeCenter), inside its own, so
// that where a ray goes on through the coarser clip's voxels (thicker) moves with the eye, smoothly, rather than with the
// clip's snaps
float4 GiMarch(GiData gi, uint level, float3 origin, float3 direction, float start, float end)
{
	float3 radiance = 0.0;
	float transmittance = 1.0;
	// what the steps read of the clip they are in, and its level: only these, rather than a copy of its GiData (its
	// cascades' tables), read field by field when the ray leaves for the next
	uint clip = gi.clip;
	uint clipCount = gi.clipCount;
	uint clipLevel = level;
	float4 volumeOrigin = gi.volumeOrigin;
	uint3 levelSize = GiLevelSize(gi, clipLevel);
	uint levelFirst = GiLevelFirst(gi, clipLevel);
	float voxelSize = volumeOrigin.w * float(1u << clipLevel);
	float boxExit = GiFadeBoxExit(gi.fadeCenter, gi.fadeExtent, origin, direction);
	float t = start;
	while (t < end && transmittance > 0.01)
	{
		float step = voxelSize * 0.5;
		float3 position = origin + direction * (t + step * 0.5);
		int3 voxel = int3(floor((position - volumeOrigin.xyz) / voxelSize));
		if (!GiInside(voxel, levelSize) || t + step * 0.5 > boxExit)
		{
			if (clip + 1 >= clipCount)
				break;
			clip++;
			boxExit = GiFadeBoxExit(gGi[gPushConstants.frameIndex][clip].fadeCenter, gGi[gPushConstants.frameIndex][clip].fadeExtent, origin, direction);
			volumeOrigin = gGi[gPushConstants.frameIndex][clip].volumeOrigin;
			uint4 volumeSize = gGi[gPushConstants.frameIndex][clip].volumeSize;
			clipLevel = min(clipLevel > 0u ? clipLevel - 1u : 0u, volumeSize.w - 1u);
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
		radiance += transmittance * sample.rgb;
		transmittance *= 1.0 - saturate(sample.a);
		t += step;
	}
	return float4(radiance, transmittance);
}

// whether the voxels let a ray through from origin, a distance along direction (level 0's occupancy: a shadow ray).
// each voxel the ray crosses visited once (Amanatides and Woo), so no thin wall is stepped over; where it leaves the clip
// it goes on through the coarser ones
float GiVisibility(GiData gi, float3 origin, float3 direction, float distance)
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
		// the distance (in voxels) along the ray to the next boundary per axis, and between boundaries
		float3 next = (select(direction >= 0.0, float3(voxel + 1) - start, start - float3(voxel))) * inverse;
		float3 delta = inverse;
		float end = (distance - travelled) / clip.volumeOrigin.w;
		// and where it leaves the clip's box around the eye (see GiMarch), in its voxels
		float boxExit = GiFadeBoxExit(clip.fadeCenter, clip.fadeExtent, origin + direction * travelled, direction) / clip.volumeOrigin.w;
		float t = 0.0;
		bool left = false;
		while (t < end)
		{
			if (!GiInside(voxel, size) || t > boxExit)
			{
				left = true;
				break;
			}
			if ((gGiVoxels[clip.voxelFirst + GiVoxelIndex(uint3(voxel), size)].albedo >> 24) != 0u)
				return 0.0;
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
			return 1.0;
		travelled += t * clip.volumeOrigin.w;
	}
	return 1.0;
}

// whether a voxel of a clip being voxelized scrolled in (wasn't in it the frame before: see GiData::scroll)
bool GiScrolledIn(GiData gi, int3 voxel)
{
	return !GiInside(voxel + gi.scroll.xyz, GiLevelSize(gi, 0));
}

// voxelizing: each voxel the static one kept at its place before the clip moved (with its mask), or empty where it
// scrolled in (all of the clip when it is voxelized again)
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
					InterlockedMax(gGiVoxels[index].albedo, voxel.albedo);
					InterlockedMax(gGiVoxels[index].normal, voxel.normal);
					InterlockedMax(gGiVoxels[index].emissionRG, voxel.emissionRG);
					InterlockedMax(gGiVoxels[index].emissionB, voxel.emissionB);
				}
			}
		}
	}
}

// lighting the voxels: each one's emission plus its albedo over pi times the light reaching it from the lights of its
// cell, each shadowed by a ray through the voxels (the shadow atlas only covers what the camera sees)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiLight(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	uint frameIndex = gPushConstants.frameIndex;
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;

	GiVoxel voxel = gGiVoxels[gi.voxelFirst + threadId.x];
	float4 albedo = GiUnpackRgba8(voxel.albedo);
	if (albedo.a == 0.0)
	{
		gGiRadiance[GiLevelFirst(gi, 0) + threadId.x] = uint2(0, 0);
		return;
	}
	uint3 at = uint3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	float3 position = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	float3 normal = GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0);
	// the shadow rays start a voxel further from the surface (the voxel is on the side it stands for, its twin across it)
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
		float visibility = GiVisibility(gi, position + normal * gi.volumeOrigin.w, toLight, distance - gi.volumeOrigin.w);
		irradiance += light.intensity.rgb * attenuation * nDotL * visibility;
	}
	float3 radiance = emission + albedo.rgb / 3.14159265 * irradiance;
	gGiRadiance[GiLevelFirst(gi, 0) + threadId.x] = GiPackRgba16(float4(radiance, 1.0));
	gGiRadiance[gi.directFirst + threadId.x] = GiPackRgba16(float4(radiance, 1.0));
}

// a level of the radiance from the one below (radiance times occupancy, and occupancy), anisotropic: per voxel 6 faces,
// one per axis direction, each what a ray travelling that way sees through it: per lane of 2 children along the axis,
// the nearer one over the farther (front to back), the 4 lanes' mean. the mean of the 8 children, the same from every
// side, let a thin surface's lit face through to the other side (the sun on the outside of Sponza's curtains and walls
// lit its arcades' columns from inside, red and yellow, which the cascades traced at level 0 didn't)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiMip(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint level = gPushConstants.giStep;
	uint3 size = GiLevelSize(gi, level);
	uint voxelCount = size.x * size.y * size.z;
	if (threadId.x >= voxelCount * 6u)
		return;
	uint face = threadId.x / voxelCount;
	uint voxelIndex = threadId.x % voxelCount;
	int3 at = int3(voxelIndex % size.x, (voxelIndex / size.x) % size.y, voxelIndex / (size.x * size.y));
	uint axis = face / 2u;
	bool negative = (face & 1u) != 0u;
	int3 step = int3(axis == 0u, axis == 1u, axis == 2u);
	float4 sum = 0.0;
	for (uint laneIt = 0; laneIt < 4; laneIt++)
	{
		int3 lane = axis == 0u ? int3(0, laneIt & 1, laneIt >> 1) : (axis == 1u ? int3(laneIt & 1, 0, laneIt >> 1) : int3(laneIt & 1, laneIt >> 1, 0));
		int3 low = at * 2 + lane;
		float4 front = GiRadianceAt(gi, level - 1, negative ? low + step : low, face);
		float4 back = GiRadianceAt(gi, level - 1, negative ? low : low + step, face);
		float opening = 1.0 - saturate(front.a);
		sum += float4(front.rgb + opening * back.rgb, front.a + opening * back.a);
	}
	gGiRadiance[GiRadianceIndex(GiLevelFirst(gi, level), level, uint3(at), size, face)] = GiPackRgba16(sum / 4.0);
}

// a cascade's probe's center (world space)
float3 GiProbePosition(GiData gi, uint cascade, uint3 probe)
{
	return gi.cascadeOrigin[cascade].xyz + (float3(probe) + 0.5) * gi.cascadeInterval[cascade].w;
}

uint GiCascadeFirst(GiData gi, uint cascade)
{
	return gi.cascadeFirst[cascade / 4][cascade % 4];
}

uint GiCascadeProbeFirst(GiData gi, uint cascade)
{
	return gi.cascadeProbeFirst[cascade / 4][cascade % 4];
}

// the nearest probe of the cascade above below a probe (of the 8 its merge interpolates), and the fraction of the way
int3 GiAboveBase(GiData gi, uint cascade, uint3 probe, out float3 fraction)
{
	float3 coordinate = (GiProbePosition(gi, cascade, probe) - gi.cascadeOrigin[cascade + 1].xyz) / gi.cascadeInterval[cascade + 1].w - 0.5;
	int3 base = int3(floor(coordinate));
	fraction = coordinate - float3(base);
	return base;
}

// tracing a cascade (giStep): a thread per probe and direction, its interval through the level of its probes' spacing
// (as far as the volume has levels). the last cascade's rays that get through see the environment
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiTrace(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint cascade = gPushConstants.giStep;
	uint4 cascadeSize = gi.cascadeSize[cascade];
	uint directionCount = cascadeSize.w * cascadeSize.w;
	uint probeCount = cascadeSize.x * cascadeSize.y * cascadeSize.z;
	if (threadId.x >= probeCount * directionCount)
		return;
	uint probeIndex = threadId.x / directionCount;
	uint directionIndex = threadId.x % directionCount;
	uint3 probe = uint3(probeIndex % cascadeSize.x, (probeIndex / cascadeSize.x) % cascadeSize.y, probeIndex / (cascadeSize.x * cascadeSize.y));
	float3 direction = GiDirection(uint2(directionIndex % cascadeSize.w, directionIndex / cascadeSize.w), cascadeSize.w);
	float4 interval = gi.cascadeInterval[cascade];
	// the level of the probes' spacing (a finer one gave 10% more light near walls, a coarse level's occupancy being the
	// mean of what it holds, at twice the cost); 7 traces level 0, for comparison
	uint level = gi.debug == 7u ? 0u : min(cascade, gi.marchLevelCount - 1);
	float4 result = GiMarch(gi, level, GiProbePosition(gi, cascade, probe), direction, interval.x, interval.y);
	if (cascade + 1 == gi.cascadeCount)
	{
		EnvironmentData environment = gEnvironment[0];
		result.rgb += result.a * EnvironmentRadiance(environment, direction, 0.6);
		result.a = 0.0;
	}
	gGiCascades[GiCascadeFirst(gi, cascade) + threadId.x] = GiPackRgba16(result);
}

// merging a cascade (giStep) with the one above it, already merged: what gets through each of its rays sees what the
// rays above it see beyond, in its 2x2 directions, at its 8 nearest probes (trilinear)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiMerge(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint cascade = gPushConstants.giStep;
	uint4 cascadeSize = gi.cascadeSize[cascade];
	uint directionCount = cascadeSize.w * cascadeSize.w;
	uint probeCount = cascadeSize.x * cascadeSize.y * cascadeSize.z;
	if (threadId.x >= probeCount * directionCount)
		return;
	uint probeIndex = threadId.x / directionCount;
	uint directionIndex = threadId.x % directionCount;
	uint3 probe = uint3(probeIndex % cascadeSize.x, (probeIndex / cascadeSize.x) % cascadeSize.y, probeIndex / (cascadeSize.x * cascadeSize.y));
	uint2 texel = uint2(directionIndex % cascadeSize.w, directionIndex / cascadeSize.w);

	uint index = GiCascadeFirst(gi, cascade) + threadId.x;
	float4 own = GiUnpackRgba16(gGiCascades[index]);
	if (own.a <= 0.0)
		return;

	uint4 aboveSize = gi.cascadeSize[cascade + 1];
	uint aboveDirections = aboveSize.w * aboveSize.w;
	uint aboveFirst = GiCascadeFirst(gi, cascade + 1);
	float3 fraction;
	int3 base = GiAboveBase(gi, cascade, probe, fraction);
	// only the probes above this one sees (an enclosed space's, a building's inside, see none of the light outside)
	uint visible = gGiProbeMasks[GiCascadeProbeFirst(gi, cascade) + probeIndex];
	float3 beyond = 0.0;
	float weightSum = 0.0;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
		uint3 above = uint3(clamp(base + offset, int3(0), int3(aboveSize.xyz) - 1));
		float3 weights = select(offset != 0, fraction, 1.0 - fraction);
		float weight = weights.x * weights.y * weights.z * (((visible >> cornerIt) & 1u) != 0u ? 1.0 : 0.0);
		weightSum += weight;
		uint aboveProbe = (above.z * aboveSize.y + above.y) * aboveSize.x + above.x;
		float3 sum = 0.0;
		for (uint childIt = 0; childIt < 4; childIt++)
		{
			uint2 child = texel * 2 + uint2(childIt & 1, childIt >> 1);
			sum += GiUnpackRgba16(gGiCascades[aboveFirst + aboveProbe * aboveDirections + child.y * aboveSize.w + child.x]).rgb;
		}
		beyond += weight * sum * 0.25;
	}
	gGiCascades[index] = GiPackRgba16(float4(own.rgb + own.a * beyond / max(weightSum, 1e-6), 0.0));
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

// cascade 0's merged radiance per probe as spherical harmonics (bands 0 to 2, each direction weighed by its share of
// the sphere: GiTexelSolidAngle): what the shading interpolates, SHADER_TYPES_GI_SH_COUNT float4 a probe (rgb)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiIrradiance(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint4 cascadeSize = gi.cascadeSize[0];
	uint probeCount = cascadeSize.x * cascadeSize.y * cascadeSize.z;
	if (threadId.x >= probeCount)
		return;
	uint directionCount = cascadeSize.w * cascadeSize.w;
	float3 sh[SHADER_TYPES_GI_SH_COUNT];
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
		sh[shIt] = 0.0;
	float basis[SHADER_TYPES_GI_SH_COUNT];
	if (gi.debug == 6u)
	{
		// the reference (SPEEDO_GI_DEBUG=6): the same voxels, but each probe's light from 512 rays through level 0 to the
		// environment, directly (seconds a frame)
		uint3 probe = uint3(threadId.x % cascadeSize.x, (threadId.x / cascadeSize.x) % cascadeSize.y, threadId.x / (cascadeSize.x * cascadeSize.y));
		float3 origin = GiProbePosition(gi, 0, probe);
		GiData last = GiClip(gi.clipCount - 1);
		float far = length(float3(last.volumeSize.xyz)) * last.volumeOrigin.w;
		const uint kRays = 512;
		EnvironmentData environment = gEnvironment[0];
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
	}
	else
	{
		for (uint directionIt = 0; directionIt < directionCount; directionIt++)
		{
			uint2 texel = uint2(directionIt % cascadeSize.w, directionIt / cascadeSize.w);
			float3 direction = GiDirection(texel, cascadeSize.w);
			float solidAngle = GiTexelSolidAngle(texel, cascadeSize.w);
			float3 radiance = GiUnpackRgba16(gGiCascades[GiCascadeFirst(gi, 0) + threadId.x * directionCount + directionIt]).rgb;
			GiShBasis(direction, basis);
			for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
				sh[shIt] += radiance * basis[shIt] * solidAngle;
		}
	}
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
		gGiIrradiance[(gi.probeFirst + threadId.x) * SHADER_TYPES_GI_SH_COUNT + shIt] = float4(sh[shIt], 0.0);
}

// whether a probe is visible from a point (level 0's voxels between them, at a voxel's steps; visiting every voxel the
// line crosses, as the shadow rays do, was too strict for probes on voxel corners, whose lines along a wall touch it)
bool GiProbeVisible(GiData gi, float3 from, float3 to)
{
	float3 offset = to - from;
	float distance = length(offset);
	if (distance <= 1e-6)
		return true;
	float3 direction = offset / distance;
	float step = gi.volumeOrigin.w;
	uint3 size = GiLevelSize(gi, 0);
	for (float t = step * 0.5; t < distance; t += step)
	{
		int3 voxel = int3(floor(GiVoxelCoordinate(gi, from + direction * t, 0)));
		if (GiInside(voxel, size) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(voxel), size)].albedo >> 24) != 0u)
			return false;
	}
	return true;
}

// the nearest cascade 0 probe below a point (of the 8 interpolated for it), and its fraction of the way to the next
int3 GiProbeBase(GiData gi, float3 position, out float3 fraction)
{
	float3 coordinate = (position - gi.cascadeOrigin[0].xyz) / gi.cascadeInterval[0].w - 0.5;
	int3 base = int3(floor(coordinate));
	fraction = coordinate - float3(base);
	return base;
}

// after voxelizing: which of the 8 probes around each voxel are visible from its center (GiVoxel::emissionB's upper
// bits). the probes are centered on voxel boundaries (every probeSpacing voxels, an even number, from half of it), so all
// of a voxel's points interpolate the same 8
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiVisibility(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	uint3 at = uint3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	// a shifted voxel keeps its mask (the clips move by whole probe cells), but where the march to its probes may reach
	// what scrolled in (two probe spacings), or where its probes are clamped, near a face of an axis it moved along
	int reach = int(2u * uint(gi.cascadeInterval[0].w / gi.volumeOrigin.w + 0.5));
	bool remake = false;
	for (uint axisIt = 0; axisIt < 3; axisIt++)
	{
		if (gi.scroll[axisIt] == 0)
			continue;
		int edge = int(size[axisIt]) - reach;
		int moved = int(at[axisIt]) + gi.scroll[axisIt];
		remake = remake || moved < reach || moved >= edge || int(at[axisIt]) < reach || int(at[axisIt]) >= edge;
	}
	if (!remake)
		return;
	float3 center = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	float3 fraction;
	int3 base = GiProbeBase(gi, center, fraction);
	uint4 cascadeSize = gi.cascadeSize[0];
	// a surface's voxel (half a voxel off it, on the side its normal faces) sees from a voxel further out that way: from
	// its center, among its surface's voxels, it saw none, and a lookup at an edge or corner, where most of its 8 voxels
	// are the surfaces', found nothing (and then took every probe, the sky's behind the wall too: the leaks along walls'
	// bases and corners)
	uint index = gi.voxelFirst + threadId.x;
	GiVoxel voxel = gGiVoxels[index];
	float3 from = center;
	if ((voxel.albedo >> 24) != 0u)
		from += GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0) * gi.volumeOrigin.w;
	uint mask = 0;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		uint3 probe = uint3(clamp(base + int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2), int3(0), int3(cascadeSize.xyz) - 1));
		if (GiProbeVisible(gi, from, GiProbePosition(gi, 0, probe)))
			mask |= 1u << cornerIt;
	}

	// what its probes miss of its near surroundings: the share of the hemisphere away from the surfaces next to it (its
	// normal, if it is one; else its occupancy's gradient) that is open as far as a probe spacing. a probe, up to a
	// spacing off a surface, sees past what is nearer the surface than it is (an arcade's walls and ceiling, a curtain's
	// folds), so the coarser a clip, the brighter it lit a surface: against the light at the surface itself (rays from
	// the point), Sponza's arcade by clip 1's probes 119%, by clip 2's 160%. the resolve scales the probes by it
	float3 away = 0.0;
	if ((voxel.albedo >> 24) != 0u)
		away = GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0);
	else
		for (uint axisIt = 0; axisIt < 3; axisIt++)
		{
			int3 step = int3(axisIt == 0u, axisIt == 1u, axisIt == 2u);
			float below = GiInside(int3(at) - step, size) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(int3(at) - step), size)].albedo >> 24) != 0u ? 1.0 : 0.0;
			float above = GiInside(int3(at) + step, size) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(int3(at) + step), size)].albedo >> 24) != 0u ? 1.0 : 0.0;
			away[axisIt] = below - above;
		}
	float open = 1.0;
	if (dot(away, away) > 0.0)
	{
		away = normalize(away);
		float3 tangent = normalize(abs(away.y) < 0.9 ? cross(away, float3(0, 1, 0)) : cross(away, float3(1, 0, 0)));
		float3 bitangent = cross(away, tangent);
		// two spacings: one left the coarser clips brighter (Sponza's arcade by clip 1 101%, by clip 2 129%; two: 96 and
		// 108%, three of them: the lit image's mean over a move of the clips from 87 to 113% of its average, now 91 to 107)
		float reach = gi.cascadeInterval[0].w * 2.0;
		const uint kRays = 16;
		uint openCount = 0;
		for (uint rayIt = 0; rayIt < kRays; rayIt++)
		{
			// cosine weighted, a Fibonacci spiral over the disk
			float u = (float(rayIt) + 0.5) / float(kRays);
			float r = sqrt(u);
			float phi = float(rayIt) * 2.39996323;
			float3 direction = tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + away * sqrt(max(1.0 - u, 0.0));
			if (GiProbeVisible(gi, from, from + direction * reach))
				openCount++;
		}
		open = float(openCount) / float(kRays);
	}
	uint openBits = uint(round(open * 255.0));
	gGiVoxels[index].emissionB = (gGiVoxels[index].emissionB & 0xFFFFu) | (mask << 16) | (openBits << 24);
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

// a bounce after the first: the voxels' direct radiance plus what their albedo reflects of the probes' irradiance at them
// (the last bounce's: the lights' and the sky's light off the scene so far)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiBounce(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	GiVoxel voxel = gGiVoxels[gi.voxelFirst + threadId.x];
	float4 albedo = GiUnpackRgba8(voxel.albedo);
	if (albedo.a == 0.0)
		return;
	uint3 at = uint3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	float3 position = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	float3 normal = GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0);
	float3 reflected = GiIrradianceResolved(gi, position, normal);
	float3 radiance = GiUnpackRgba16(gGiRadiance[gi.directFirst + threadId.x]).rgb + albedo.rgb * max(reflected, 0.0);
	gGiRadiance[GiLevelFirst(gi, 0) + threadId.x] = GiPackRgba16(float4(radiance, 1.0));
}

// after voxelizing, per probe of a cascade (giStep, all but the last): which of the 8 probes of the one above around it
// it sees (a march through the voxels between them), for the merge
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiProbeVisibility(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint cascade = gPushConstants.giStep;
	uint4 cascadeSize = gi.cascadeSize[cascade];
	if (threadId.x >= cascadeSize.x * cascadeSize.y * cascadeSize.z)
		return;
	uint3 probe = uint3(threadId.x % cascadeSize.x, (threadId.x / cascadeSize.x) % cascadeSize.y, threadId.x / (cascadeSize.x * cascadeSize.y));
	uint4 aboveSize = gi.cascadeSize[cascade + 1];
	float3 fraction;
	int3 base = GiAboveBase(gi, cascade, probe, fraction);
	float3 position = GiProbePosition(gi, cascade, probe);
	uint mask = 0;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		uint3 above = uint3(clamp(base + int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2), int3(0), int3(aboveSize.xyz) - 1));
		if (GiProbeVisible(gi, position, GiProbePosition(gi, cascade + 1, above)))
			mask |= 1u << cornerIt;
	}
	gGiProbeMasks[GiCascadeProbeFirst(gi, cascade) + threadId.x] = mask;
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
// irradiance of the probes it sees, weighed by how many it sees (see ComputeGiResolve). a negative value where they see
// none; with weightOnly, the weight
float3 GiIrradianceResolved(GiData gi, float3 position, float3 normal, bool weightOnly = false)
{
	GiSample sample = GiSampleClip(GiClipViewAt(gi.clip), position, normal, normal * 1.5);
	if (weightOnly)
		return sample.last.w;
	return sample.valid ? GiEvaluate(sample, normal) : -1.0;
}

// per voxel of a clip, after its probes' irradiance: the spherical harmonics of the 8 probes around its center that it
// sees (its mask, see ComputeGiVisibility), trilinear by its center, times the share of that weight they hold, and the
// share. a point interpolating the voxels around it so sees each probe as much as the voxels around it do (one voxel's
// mask switched at its edges, which showed as blocks where voxels are large), at 4 filtered reads rather than the masks of
// 8 voxels for each of 8 probes (which cost Bistro's main pass 4 ms up close)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiResolve(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	uint3 at = uint3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	float3 center = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	uint visibility = gGiVoxels[gi.voxelFirst + threadId.x].emissionB;
	uint visible = (visibility >> 16) & 0xFFu;
	// the share of its near surroundings open (see ComputeGiVisibility)
	float open = float(visibility >> 24) / 255.0;
	float3 fraction;
	int3 base = GiProbeBase(gi, center, fraction);
	uint4 cascadeSize = gi.cascadeSize[0];
	float3 sh[SHADER_TYPES_GI_SH_COUNT];
	for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
		sh[shIt] = 0.0;
	float seen = 0.0;
	float share = 1.0;
	// which way is away from the surfaces next to it (the occupancy's gradient over its 6 neighbors): the probes that
	// way weigh more, as the per pixel lookups weighed them by how much they faced the surface (Sponza's inside 79% of
	// its reference's light without, 83% with)
	float3 away = 0.0;
	for (uint axisIt = 0; axisIt < 3; axisIt++)
	{
		int3 step = int3(axisIt == 0u, axisIt == 1u, axisIt == 2u);
		float below = GiInside(int3(at) - step, size) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(int3(at) - step), size)].albedo >> 24) != 0u ? 1.0 : 0.0;
		float above = GiInside(int3(at) + step, size) && (gGiVoxels[gi.voxelFirst + GiVoxelIndex(uint3(int3(at) + step), size)].albedo >> 24) != 0u ? 1.0 : 0.0;
		away[axisIt] = below - above;
	}
	float awayLength = length(away);
	away = awayLength > 0.0 ? away / awayLength : away;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		if (((visible >> cornerIt) & 1u) == 0u)
			continue;
		int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
		uint3 probe = uint3(clamp(base + offset, int3(0), int3(cascadeSize.xyz) - 1));
		float3 weights = select(offset != 0, fraction, 1.0 - fraction);
		float weight = weights.x * weights.y * weights.z * share;
		if (awayLength > 0.0)
		{
			float facing = dot(normalize(GiProbePosition(gi, 0, probe) - center), away) * 0.5 + 0.5;
			weight *= facing * facing + 0.05;
		}
		uint probeIndex = gi.probeFirst + (probe.z * cascadeSize.y + probe.y) * cascadeSize.x + probe.x;
		for (uint shIt = 0; shIt < SHADER_TYPES_GI_SH_COUNT; shIt++)
			sh[shIt] += (weight * open) * gGiIrradiance[probeIndex * SHADER_TYPES_GI_SH_COUNT + shIt].rgb;
		seen += weight;
	}
	// per color its coefficients 0 to 3 and 4 to 7, then the three colors' coefficient 8 and the weight (see GiSample)
	uint3 texel = uint3(at.x, at.y, at.z + gi.clip * size.z);
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
// sight, which is clear: at a concave edge the normal alone leads into the other surface's voxels, which held nothing
// before they saw from their front, see ComputeGiVisibility, and a third of Sponza's arcade then had none), and twice as
// far where that finds no probes
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
// the point drawn over it: its voxels' thin, in the clip's color, and its cascade 0 probes' (the planes through them,
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
// voxel it meets holds (its radiance as the cascades read it, from the levels' level 0; its albedo; its normal); the
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

// the debug view of the probes (DEBUG_VIEW_GI_PROBES): the 8 cascade 0 probes around the point of the finest clip that
// holds it, trilinear, each its irradiance along the normal as GiEvaluate has it (the cosine lobe over pi)
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
		uint4 cascadeSize = gi.cascadeSize[0];
		float4 basis0 = float4(0.282095, 2.0 / 3.0 * 0.488603 * normal);
		float3 d = normal;
		float4 basis1 = 0.25 * float4(1.092548 * d.x * d.y, 1.092548 * d.y * d.z, 0.315392 * (3.0 * d.z * d.z - 1.0), 1.092548 * d.x * d.z);
		float basis8 = 0.25 * 0.546274 * (d.x * d.x - d.y * d.y);
		float3 result = 0.0;
		for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		{
			int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
			uint3 probe = uint3(clamp(base + offset, int3(0), int3(cascadeSize.xyz) - 1));
			float3 weights = select(offset != 0, fraction, 1.0 - fraction);
			uint first = (gi.probeFirst + (probe.z * cascadeSize.y + probe.y) * cascadeSize.x + probe.x) * SHADER_TYPES_GI_SH_COUNT;
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
