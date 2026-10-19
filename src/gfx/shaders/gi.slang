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

// a level's radiance (times occupancy) and occupancy at a voxel, 0 outside the volume
float4 GiRadianceAt(GiData gi, uint level, int3 voxel)
{
	uint3 size = GiLevelSize(gi, level);
	if (!GiInside(voxel, size))
		return 0.0;
	return GiUnpackRgba16(gGiRadiance[GiLevelFirst(gi, level) + GiVoxelIndex(uint3(voxel), size)]);
}

// along a ray through a level of the volume, from start to end (world space distances): the radiance reaching its
// origin (front to back) and how much of what is beyond gets through
float4 GiMarch(GiData gi, uint level, float3 origin, float3 direction, float start, float end)
{
	float voxelSize = gi.volumeOrigin.w * float(1u << level);
	float step = voxelSize * 0.5;
	float3 radiance = 0.0;
	float transmittance = 1.0;
	for (float t = start + step * 0.5; t < end && transmittance > 0.01; t += step)
	{
		float4 sample = GiRadianceAt(gi, level, int3(floor(GiVoxelCoordinate(gi, origin + direction * t, level))));
		radiance += transmittance * sample.rgb;
		transmittance *= 1.0 - saturate(sample.a);
	}
	return float4(radiance, transmittance);
}

// whether the voxels let a ray through from origin, a distance along direction (level 0's occupancy: a shadow ray).
// each voxel the ray crosses visited once (Amanatides and Woo), so no thin wall is stepped over
float GiVisibility(GiData gi, float3 origin, float3 direction, float distance)
{
	uint3 size = GiLevelSize(gi, 0);
	float3 start = GiVoxelCoordinate(gi, origin, 0);
	int3 voxel = int3(floor(start));
	int3 stepDirection = int3(select(direction >= 0.0, 1, -1));
	float3 inverse = 1.0 / max(abs(direction), 1e-8);
	// the distance (in voxels) along the ray to the next boundary per axis, and between boundaries
	float3 next = (select(direction >= 0.0, float3(voxel + 1) - start, start - float3(voxel))) * inverse;
	float3 delta = inverse;
	float end = distance / gi.volumeOrigin.w;
	float t = 0.0;
	while (t < end)
	{
		if (!GiInside(voxel, size))
			return 1.0;
		if ((gGiVoxels[GiVoxelIndex(uint3(voxel), size)].albedo >> 24) != 0u)
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
	return 1.0;
}

// voxelizing: empties the voxels
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiClear(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = gGi[gPushConstants.frameIndex][0];
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	gGiVoxels[threadId.x] = (GiVoxel)0;
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
	GiData gi = gGi[frameIndex][0];
	uint jobCount = gPushConstants.giStep;
	// past the last job's triangles (gGiJobs ends with the triangle count)
	if (threadId.x >= gGiJobs[frameIndex][jobCount].x)
		return;
	// the job: the last whose first triangle is at most this one
	uint low = 0;
	uint high = jobCount;
	while (high - low > 1)
	{
		uint middle = (low + high) / 2;
		if (gGiJobs[frameIndex][middle].x <= threadId.x)
			low = middle;
		else
			high = middle;
	}
	uint4 job = gGiJobs[frameIndex][low];

	uint triangle = threadId.x - job.x;
	DrawRecord record = gDrawRecords[frameIndex][job.z];
	MaterialData material = gMaterialData[record.materialId];
	VertexP3fN3fTa4fT014fC4f vertices[3];
	for (uint cornerIt = 0; cornerIt < 3; cornerIt++)
		vertices[cornerIt] = gVertexBuffer[gIndexBuffer[job.y + triangle * 3 + cornerIt]];

	// the triangle's albedo and emission: the material's, with its textures at the centroid, at a level of about a sample's
	// size
	float4 color = (vertices[0].color + vertices[1].color + vertices[2].color) / 3.0 * material.color;
	float4 texCoord01 = (vertices[0].texCoord01 + vertices[1].texCoord01 + vertices[2].texCoord01) / 3.0;
	if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
	{
		TextureView view = gTextureViews[material.baseColorView];
		color *= gTextures[view.textureId].SampleLevel(gSamplers[view.samplerId], ViewTexCoord(view, texCoord01), 6.0);
	}
	float3 albedo = color.rgb * (1.0 - material.metallic);
	float3 emission = material.emissive.rgb;
	if ((material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
	{
		TextureView view = gTextureViews[material.emissiveView];
		emission *= gTextures[view.textureId].SampleLevel(gSamplers[view.samplerId], ViewTexCoord(view, texCoord01), 6.0).rgb;
	}

	uint3 size = GiLevelSize(gi, 0);
	uint instanceCount = job.w & 0x7FFFFFFFu;
	for (uint instanceIt = 0; instanceIt < instanceCount; instanceIt++)
	{
		float4x4 transform = gModelInstances[frameIndex][record.modelInstanceId + instanceIt].modelTransform;
		float3 p0 = mul(transform, float4(vertices[0].position, 1.0)).xyz;
		float3 p1 = mul(transform, float4(vertices[1].position, 1.0)).xyz;
		float3 p2 = mul(transform, float4(vertices[2].position, 1.0)).xyz;
		float3 normal = cross(p1 - p0, p2 - p0);
		if (dot(normal, normal) <= 0.0)
			continue;
		normal = normalize(normal);
		float longest = max(length(p1 - p0), max(length(p2 - p1), length(p0 - p2)));
		uint steps = clamp(uint(ceil(longest / (gi.volumeOrigin.w * 0.5))), 1u, 512u);
		// a voxel behind each of its faces, half of one off it (one on a voxel boundary would round to either side),
		// facing away from it: which face is seen and lit isn't known here (a double sided material's either, and a
		// triangle's winding, its vertex normals and which side it is drawn from needn't agree)
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
			for (uint a = 0; a <= steps; a++)
				for (uint b = 0; a + b <= steps; b++)
				{
					float3 p = p0 + (p1 - p0) * (float(a) / float(steps)) + (p2 - p0) * (float(b) / float(steps)) -
							   facing * (offset * gi.volumeOrigin.w);
					int3 at = int3(floor(GiVoxelCoordinate(gi, p, 0)));
					if (!GiInside(at, size))
						continue;
					uint index = GiVoxelIndex(uint3(at), size);
					InterlockedMax(gGiVoxels[index].albedo, voxel.albedo);
					InterlockedMax(gGiVoxels[index].normal, voxel.normal);
					InterlockedMax(gGiVoxels[index].emissionRG, voxel.emissionRG);
					InterlockedMax(gGiVoxels[index].emissionB, voxel.emissionB);
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
	GiData gi = gGi[frameIndex][0];
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;

	GiVoxel voxel = gGiVoxels[threadId.x];
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
	uint2 range = gGiLightCells[frameIndex][(cell.z * SHADER_TYPES_GI_LIGHT_GRID + cell.y) * SHADER_TYPES_GI_LIGHT_GRID + cell.x];
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
	GiData gi = gGi[gPushConstants.frameIndex][0];
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
	GiData gi = gGi[gPushConstants.frameIndex][0];
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
	GiData gi = gGi[gPushConstants.frameIndex][0];
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
	GiData gi = gGi[gPushConstants.frameIndex][0];
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
		float far = length(float3(gi.volumeSize.xyz)) * gi.volumeOrigin.w;
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
		gGiIrradiance[threadId.x * 3 + 0] = r;
		gGiIrradiance[threadId.x * 3 + 1] = g;
		gGiIrradiance[threadId.x * 3 + 2] = b;
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
	gGiIrradiance[threadId.x * 3 + 0] = r;
	gGiIrradiance[threadId.x * 3 + 1] = g;
	gGiIrradiance[threadId.x * 3 + 2] = b;
}

// whether a probe is visible from a point (level 0's voxels between them, at a voxel's steps)
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
		if (GiInside(voxel, size) && (gGiVoxels[GiVoxelIndex(uint3(voxel), size)].albedo >> 24) != 0u)
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
	GiData gi = gGi[gPushConstants.frameIndex][0];
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	uint3 at = uint3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
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
	gGiVoxels[threadId.x].emissionB = (gGiVoxels[threadId.x].emissionB & 0xFFFFu) | (mask << 16);
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

// the radiance a white Lambertian surface at position, of normal, reflects by the probes (as EnvironmentIrradiance:
// the irradiance over pi), or a negative value where the probes don't cover it. the 8 nearest probes of cascade 0,
// trilinear, from the point offset along its normal by one and a half voxels (off its own surface's voxels, a staircase
// where it is diagonal to them), each weighed by whether it is visible from there (the voxel's, see ComputeGiVisibility)
// and how much it faces the surface
float3 GiIrradiance(float3 position, float3 normal, uint debug)
{
	GiData gi = gGi[gPushConstants.frameIndex][0];
	if (gi.enabled == 0)
		return -1.0;
	uint4 cascadeSize = gi.cascadeSize[0];
	float3 biased = position + normal * (1.5 * gi.volumeOrigin.w);
	float3 fraction;
	int3 base = GiProbeBase(gi, biased, fraction);
	uint3 levelSize = GiLevelSize(gi, 0);
	int3 voxel = int3(floor(GiVoxelCoordinate(gi, biased, 0)));
	if (!GiInside(voxel, levelSize))
		return -1.0;
	uint visible = gGiVoxels[GiVoxelIndex(uint3(voxel), levelSize)].emissionB >> 16;
	// each probe's visibility from the 8 voxels around the point, trilinear by their centers (one voxel's mask switches
	// at its edges, which shows where voxels are large): a voxel's mask says only for the 8 probes around it
	// (the bounce's lookups, debug 8, take the voxel's mask alone: their precision matters less than their cost)
	bool soft = debug != 8u;
	float3 voxelCoordinate = GiVoxelCoordinate(gi, biased, 0) - 0.5;
	int3 voxelBase = int3(floor(voxelCoordinate));
	float3 voxelFraction = voxelCoordinate - float3(voxelBase);
	float visibility[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
	float coverage[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
	for (uint neighborIt = 0; neighborIt < (soft ? 8u : 0u); neighborIt++)
	{
		int3 neighborOffset = int3(neighborIt & 1, (neighborIt >> 1) & 1, neighborIt >> 2);
		int3 neighbor = voxelBase + neighborOffset;
		if (!GiInside(neighbor, levelSize))
			continue;
		float3 neighborWeights = select(neighborOffset != 0, voxelFraction, 1.0 - voxelFraction);
		float neighborWeight = neighborWeights.x * neighborWeights.y * neighborWeights.z;
		float3 unused;
		int3 neighborBase = GiProbeBase(gi, gi.volumeOrigin.xyz + (float3(neighbor) + 0.5) * gi.volumeOrigin.w, unused);
		uint neighborMask = gGiVoxels[GiVoxelIndex(uint3(neighbor), levelSize)].emissionB >> 16;
		for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		{
			int3 relative = base + int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2) - neighborBase;
			if (any(relative < 0) || any(relative > 1))
				continue;
			uint bit = uint(relative.x) | (uint(relative.y) << 1) | (uint(relative.z) << 2);
			visibility[cornerIt] += neighborWeight * float((neighborMask >> bit) & 1u);
			coverage[cornerIt] += neighborWeight;
		}
	}
	float3 sum = 0.0;
	float weightSum = 0.0;
	float4 basis = float4(3.14159265 * 0.282095, 2.0 * 3.14159265 / 3.0 * 0.488603 * normal);
	for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
	{
		int3 offset = int3(cornerIt & 1, (cornerIt >> 1) & 1, cornerIt >> 2);
		uint3 probe = uint3(clamp(base + offset, int3(0), int3(cascadeSize.xyz) - 1));
		float3 weights = select(offset != 0, fraction, 1.0 - fraction);
		float weight = weights.x * weights.y * weights.z;
		float facing = dot(normalize(GiProbePosition(gi, 0, probe) - position), normal) * 0.5 + 0.5;
		weight *= facing * facing + 0.05;
		float seen = coverage[cornerIt] > 0.0 ? visibility[cornerIt] / coverage[cornerIt] : float((visible >> cornerIt) & 1u);
		weight *= max(seen, 1e-3);
		uint probeIndex = (probe.z * cascadeSize.y + probe.y) * cascadeSize.x + probe.x;
		float3 irradiance = float3(
			dot(gGiIrradiance[probeIndex * 3 + 0], basis), dot(gGiIrradiance[probeIndex * 3 + 1], basis), dot(gGiIrradiance[probeIndex * 3 + 2], basis));
		sum += weight * max(irradiance, 0.0);
		weightSum += weight;
	}
	if (debug == 2u)
		return float(countbits(visible)) / 8.0;
	if (debug >= 3u && debug <= 5u)
	{
		// the nearest voxel behind the point (or in front, 4), along its normal
		float side = debug == 4u ? 1.0 : -1.0;
		for (float t = 0.25; t <= 1.75; t += 0.5)
		{
			int3 at = int3(floor(GiVoxelCoordinate(gi, position + normal * (side * t * gi.volumeOrigin.w), 0)));
			if (!GiInside(at, levelSize))
				return float3(0.0, 1.0, 1.0);
			GiVoxel found = gGiVoxels[GiVoxelIndex(uint3(at), levelSize)];
			if ((found.albedo >> 24) == 0u)
				continue;
			if (debug == 5u)
				return GiUnpackRgba8(found.albedo).rgb;
			return GiRadianceAt(gi, 0, at).rgb;
		}
		return float3(1.0, 0.0, 1.0);
	}
	if (weightSum <= 1e-6)
		return -1.0;
	return sum / weightSum / 3.14159265;
}

// a bounce after the first: the voxels' direct radiance plus what their albedo reflects of the probes' irradiance at them
// (the last bounce's: the lights' and the sky's light off the scene so far)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiBounce(uint3 dispatchId: SV_DispatchThreadID)
{
	uint3 threadId = GiThread(dispatchId);
	GiData gi = gGi[gPushConstants.frameIndex][0];
	uint3 size = GiLevelSize(gi, 0);
	if (threadId.x >= size.x * size.y * size.z)
		return;
	GiVoxel voxel = gGiVoxels[threadId.x];
	float4 albedo = GiUnpackRgba8(voxel.albedo);
	if (albedo.a == 0.0)
		return;
	uint3 at = uint3(threadId.x % size.x, (threadId.x / size.x) % size.y, threadId.x / (size.x * size.y));
	float3 position = gi.volumeOrigin.xyz + (float3(at) + 0.5) * gi.volumeOrigin.w;
	float3 normal = GiFromOctahedral(float2(voxel.normal & 0xFFFFu, voxel.normal >> 16) / 65535.0 * 2.0 - 1.0);
	float3 reflected = GiIrradiance(position, normal, 8);
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
	GiData gi = gGi[gPushConstants.frameIndex][0];
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
