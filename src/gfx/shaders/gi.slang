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
uint3 GiLevelSize(GiData gi, uint level)
{
	return max(gi.volumeSize.xyz >> level, uint3(1));
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

// a clip's voxel (level 0)
GiVoxel GiVoxelAt(GiData gi, uint3 voxel)
{
	return gGiVoxels[gi.voxelFirst + GiVoxelIndex(voxel, GiLevelSize(gi, 0))];
}

// a level's radiance (times occupancy) and occupancy at a voxel, 0 outside the volume
float4 GiRadianceAt(GiData gi, uint level, int3 voxel)
{
	uint3 size = GiLevelSize(gi, level);
	if (!GiInside(voxel, size))
		return 0.0;
	return GiUnpackRgba16(gGiRadiance[GiLevelFirst(gi, level) + GiVoxelIndex(uint3(voxel), size)]);
}

// along a ray through a level of the volume, from start to end (world space distances): the radiance reaching its
// origin (front to back) and how much of what is beyond gets through. where it leaves the clip it goes on through the
// coarser ones (never back: they are boxes), at their level of the same voxel size (or level 0), at half their voxels'
// steps
float4 GiMarch(GiData gi, uint level, float3 origin, float3 direction, float start, float end)
{
	float3 radiance = 0.0;
	float transmittance = 1.0;
	GiData clip = gi;
	uint clipLevel = level;
	float t = start;
	while (t < end && transmittance > 0.01)
	{
		float step = clip.volumeOrigin.w * float(1u << clipLevel) * 0.5;
		float3 position = origin + direction * (t + step * 0.5);
		int3 voxel = int3(floor(GiVoxelCoordinate(clip, position, clipLevel)));
		if (!GiInside(voxel, GiLevelSize(clip, clipLevel)))
		{
			if (clip.clip + 1 >= clip.clipCount)
				break;
			clip = GiClip(clip.clip + 1);
			clipLevel = min(clipLevel > 0u ? clipLevel - 1u : 0u, clip.volumeSize.w - 1u);
			continue;
		}
		float4 sample = GiRadianceAt(clip, clipLevel, voxel);
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
		GiData clip = clipIt == gi.clip ? gi : GiClip(clipIt);
		uint3 size = GiLevelSize(clip, 0);
		float3 start = GiVoxelCoordinate(clip, origin + direction * travelled, 0);
		int3 voxel = int3(floor(start));
		int3 stepDirection = int3(select(direction >= 0.0, 1, -1));
		float3 inverse = 1.0 / max(abs(direction), 1e-8);
		// the distance (in voxels) along the ray to the next boundary per axis, and between boundaries
		float3 next = (select(direction >= 0.0, float3(voxel + 1) - start, start - float3(voxel))) * inverse;
		float3 delta = inverse;
		float end = (distance - travelled) / clip.volumeOrigin.w;
		float t = 0.0;
		bool left = false;
		while (t < end)
		{
			if (!GiInside(voxel, size))
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
			albedo = color.rgb * (1.0 - material.metallic);
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
			float2 octahedral = GiOctahedral(facing) * 0.5 + 0.5;
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
	// the shadow rays start a voxel off the surface, in front of it (the voxels are behind it)
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

// a level of the radiance from the one below: each voxel the mean of its 8 (radiance times occupancy, and occupancy)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiMip(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = GiClip(gPushConstants.giClip);
	uint level = gPushConstants.giStep;
	uint3 size = GiLevelSize(gi, level);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	int3 at = int3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	float4 sum = 0.0;
	for (uint childIt = 0; childIt < 8; childIt++)
		sum += GiRadianceAt(gi, level - 1, at * 2 + int3(childIt & 1, (childIt >> 1) & 1, childIt >> 2));
	gGiRadiance[GiLevelFirst(gi, level) + threadId.x] = GiPackRgba16(sum / 8.0);
}

// a cascade's probe's center (world space)
float3 GiProbePosition(GiData gi, uint cascade, uint3 probe)
{
	return gi.volumeOrigin.xyz + (float3(probe) + 0.5) * gi.cascadeInterval[cascade].w;
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
	float3 coordinate = (GiProbePosition(gi, cascade, probe) - gi.volumeOrigin.xyz) / gi.cascadeInterval[cascade + 1].w - 0.5;
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
	uint level = gi.debug == 7u ? 0u : min(cascade, gi.volumeSize.w - 1);
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
		float weight = weights.x * weights.y * weights.z * (((visible >> cornerIt) & 1u) != 0u ? 1.0 : 1e-3);
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

// cascade 0's merged radiance per probe as spherical harmonics (bands 0 and 1, each direction the same share of the
// sphere): what the shading interpolates
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
	float4 r = 0.0;
	float4 g = 0.0;
	float4 b = 0.0;
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
			float4 basis = float4(0.282095, 0.488603 * direction) * (4.0 * 3.14159265 / float(kRays));
			r += radiance.r * basis;
			g += radiance.g * basis;
			b += radiance.b * basis;
		}
		gGiIrradiance[(gi.probeFirst + threadId.x) * 3 + 0] = r;
		gGiIrradiance[(gi.probeFirst + threadId.x) * 3 + 1] = g;
		gGiIrradiance[(gi.probeFirst + threadId.x) * 3 + 2] = b;
		return;
	}
	for (uint directionIt = 0; directionIt < directionCount; directionIt++)
	{
		uint2 texel = uint2(directionIt % cascadeSize.w, directionIt / cascadeSize.w);
		float3 direction = GiDirection(texel, cascadeSize.w);
		float solidAngle = GiTexelSolidAngle(texel, cascadeSize.w);
		float3 radiance = GiUnpackRgba16(gGiCascades[GiCascadeFirst(gi, 0) + threadId.x * directionCount + directionIt]).rgb;
		float4 basis = float4(0.282095, 0.488603 * direction) * solidAngle;
		r += radiance.r * basis;
		g += radiance.g * basis;
		b += radiance.b * basis;
	}
	gGiIrradiance[(gi.probeFirst + threadId.x) * 3 + 0] = r;
	gGiIrradiance[(gi.probeFirst + threadId.x) * 3 + 1] = g;
	gGiIrradiance[(gi.probeFirst + threadId.x) * 3 + 2] = b;
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
	float3 coordinate = (position - gi.volumeOrigin.xyz) / gi.cascadeInterval[0].w - 0.5;
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
	uint mask = 0;
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		uint3 probe = uint3(clamp(base + int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2), int3(0), int3(cascadeSize.xyz) - 1));
		if (GiProbeVisible(gi, center, GiProbePosition(gi, 0, probe)))
			mask |= 1u << cornerIt;
	}
	uint index = gi.voxelFirst + threadId.x;
	gGiVoxels[index].emissionB = (gGiVoxels[index].emissionB & 0xFFFFu) | (mask << 16);
}

// the debug view (GiData::debug), or 0
uint GiDebug()
{
	return gGi[gPushConstants.frameIndex][0].debug;
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
float3 GiIrradiance(float3 position, float3 normal, uint debug)
{
	if (debug < 2u || debug > 5u)
	{
		GiSample sample = GiSampleAt(position, normal);
		return sample.valid ? GiEvaluate(sample, normal) : -1.0;
	}
	GiData gi = GiClip(0);
	if (gi.enabled == 0)
		return -1.0;
	for (uint clipIt = 0; clipIt < gi.clipCount; clipIt++)
	{
		GiData clip = clipIt == 0 ? gi : GiClip(clipIt);
		float3 coordinate = GiVoxelCoordinate(clip, position, 0);
		float3 size = float3(GiLevelSize(clip, 0));
		float edge = min(min(coordinate.x, coordinate.y), min(coordinate.z, min(min(size.x - coordinate.x, size.y - coordinate.y), size.z - coordinate.z)));
		float spacing = clip.cascadeInterval[0].w / clip.volumeOrigin.w;
		float weight = saturate((edge - spacing) / 8.0);
		if (clipIt + 1 == gi.clipCount)
			return edge > 0.0 ? GiIrradianceAt(clip, position, normal, debug) : -1.0;
		if (weight <= 0.0)
			continue;
		float3 fine = GiIrradianceAt(clip, position, normal, debug);
		if (weight >= 1.0 || (debug >= 2u && debug <= 5u))
			return fine;
		float3 coarse = GiIrradianceAt(GiClip(clipIt + 1), position, normal, debug);
		if (any(fine < 0.0))
			return coarse;
		if (any(coarse < 0.0))
			return fine;
		return lerp(coarse, fine, weight);
	}
	return -1.0;
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
// none
float3 GiIrradianceResolved(GiData gi, float3 position, float3 normal, bool weightOnly = false)
{
	float3 biased = position + normal * (1.5 * gi.volumeOrigin.w);
	float3 size = float3(GiLevelSize(gi, 0));
	// the clip's slab of the stacked volumes, clamped inside it (no filtering across into the next)
	float3 coordinate = GiVoxelCoordinate(gi, biased, 0);
	coordinate.z = clamp(coordinate.z, 0.5, size.z - 0.5) + float(gi.clip) * size.z;
	float3 uvw = coordinate / float3(size.xy, size.z * float(gi.clipCount));
	SamplerState linear = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	float weightSum = gGiVolumes[3].SampleLevel(linear, uvw, 0.0).x;
	if (weightOnly)
		return weightSum;
	if (weightSum <= 1e-7)
		return -1.0;
	float4 r = gGiVolumes[0].SampleLevel(linear, uvw, 0.0);
	float4 g = gGiVolumes[1].SampleLevel(linear, uvw, 0.0);
	float4 b = gGiVolumes[2].SampleLevel(linear, uvw, 0.0);
	float4 basis = float4(3.14159265 * 0.282095, 2.0 * 3.14159265 / 3.0 * 0.488603 * normal);
	return max(float3(dot(r, basis), dot(g, basis), dot(b, basis)), 0.0) / weightSum / 3.14159265;
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
	uint visible = gGiVoxels[gi.voxelFirst + threadId.x].emissionB >> 16;
	float3 fraction;
	int3 base = GiProbeBase(gi, center, fraction);
	uint4 cascadeSize = gi.cascadeSize[0];
	float4 r = 0.0;
	float4 g = 0.0;
	float4 b = 0.0;
	float seen = 0.0;
	// a voxel that sees none of them (in a corner, or inside a wall) takes them all, a thousandth as much: a point whose
	// voxels see none still gets their light, as the per pixel lookups did
	float share = 1.0;
	if (visible == 0u)
	{
		visible = 0xFFu;
		share = 1e-3;
	}
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
		r += weight * gGiIrradiance[probeIndex * 3 + 0];
		g += weight * gGiIrradiance[probeIndex * 3 + 1];
		b += weight * gGiIrradiance[probeIndex * 3 + 2];
		seen += weight;
	}
	uint3 texel = uint3(at.x, at.y, at.z + gi.clip * size.z);
	gGiVolumeImages[0][texel] = r;
	gGiVolumeImages[1][texel] = g;
	gGiVolumeImages[2][texel] = b;
	gGiVolumeImages[3][texel] = float4(seen, 0.0, 0.0, 0.0);
}

// the irradiance's spherical harmonics (bands 0 and 1) per color around a point, from its clips' voxels (see
// GiSampleAt), and whether any covered it
struct GiSample
{
	float4 r;
	float4 g;
	float4 b;
	bool valid;
};

// a clip's voxels' resolved probes around the point offset 1.5 voxels along its normal (see GiIrradianceResolved),
// normalized by their weight
GiSample GiSampleClip(GiData gi, float3 position, float3 normal)
{
	GiSample result;
	float3 biased = position + normal * (1.5 * gi.volumeOrigin.w);
	float3 size = float3(GiLevelSize(gi, 0));
	float3 coordinate = GiVoxelCoordinate(gi, biased, 0);
	coordinate.z = clamp(coordinate.z, 0.5, size.z - 0.5) + float(gi.clip) * size.z;
	float3 uvw = coordinate / float3(size.xy, size.z * float(gi.clipCount));
	SamplerState linear = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	float weightSum = gGiVolumes[3].SampleLevel(linear, uvw, 0.0).x;
	result.valid = weightSum > 1e-7;
	float scale = result.valid ? 1.0 / weightSum : 0.0;
	result.r = gGiVolumes[0].SampleLevel(linear, uvw, 0.0) * scale;
	result.g = gGiVolumes[1].SampleLevel(linear, uvw, 0.0) * scale;
	result.b = gGiVolumes[2].SampleLevel(linear, uvw, 0.0) * scale;
	return result;
}

// the finest clip that holds the point well inside, blended into the next at its edge (see GiIrradiance)
GiSample GiSampleAt(float3 position, float3 normal)
{
	GiSample none;
	none.r = 0.0;
	none.g = 0.0;
	none.b = 0.0;
	none.valid = false;
	GiData gi = GiClip(0);
	if (gi.enabled == 0)
		return none;
	for (uint clipIt = 0; clipIt < gi.clipCount; clipIt++)
	{
		GiData clip = clipIt == 0 ? gi : GiClip(clipIt);
		float3 coordinate = GiVoxelCoordinate(clip, position, 0);
		float3 size = float3(GiLevelSize(clip, 0));
		float edge = min(min(coordinate.x, coordinate.y), min(coordinate.z, min(min(size.x - coordinate.x, size.y - coordinate.y), size.z - coordinate.z)));
		float spacing = clip.cascadeInterval[0].w / clip.volumeOrigin.w;
		float weight = saturate((edge - spacing) / 8.0);
		if (clipIt + 1 == gi.clipCount)
			return edge > 0.0 ? GiSampleClip(clip, position, normal) : none;
		if (weight <= 0.0)
			continue;
		GiSample fine = GiSampleClip(clip, position, normal);
		if (weight >= 1.0)
			return fine;
		GiSample coarse = GiSampleClip(GiClip(clipIt + 1), position, normal);
		if (!fine.valid)
			return coarse;
		if (!coarse.valid)
			return fine;
		fine.r = lerp(coarse.r, fine.r, weight);
		fine.g = lerp(coarse.g, fine.g, weight);
		fine.b = lerp(coarse.b, fine.b, weight);
		return fine;
	}
	return none;
}

// a sample's irradiance over pi (the radiance a white Lambertian surface reflects) for a surface facing direction: along
// a reflection instead, the radiance around it in a cosine lobe (a rough reflection's)
float3 GiEvaluate(GiSample sample, float3 direction)
{
	float4 basis = float4(0.282095, 2.0 / 3.0 * 0.488603 * direction);
	return max(float3(dot(sample.r, basis), dot(sample.g, basis), dot(sample.b, basis)), 0.0);
}
