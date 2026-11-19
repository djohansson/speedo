// the world cache (docs/world-cache-plan.md; GiData::worldOn, SPEEDO_GI_WORLD=1): the indirect lighting's light in cells
// anchored in the world at several resolutions rather than per clip of the clipmap, beside it. a hash table of cells
// (gGiWorld, GiData::worldCellCount, a power of two), each a level (its side GiData::worldCellSide times 2 to the level),
// a place (the world position over that side, floored) and a normal bucket (the axis a surface faces most, either way,
// so that the two sides of a thin wall are cells apart; the air's a bucket of its own), found by a hash and linear
// probing, its key a checksum of the three. every sample (the surface cache's rays', GiCacheUpdate) is added to its cell
// at every level, so that a coarse level's mean is the same samples' over a wider footprint and the levels agree; many
// rays add to a cell at once, so a frame's samples go into sums (fixed point, 32 bit atomic adds, rounded at random)
// in a slot the cell takes for the frame, which ComputeGiWorldFold folds into the cell's running mean once the frame's
// rays are done. included by gi.slang after the surface cache's packing (GiCachePackFaces, GiHalfDithered), before
// what reads it (GiIrradiance's debug view)

// a cell's words (GI_WORLD_CELL_WORD_COUNT): its key (0: empty), the frame it was last added to (GiData::worldFrame), the
// samples its mean is of, a spare, its faces as halves (9 words, as a cache entry's: an ambient cube's light along the
// axes, GiCachePackFaces), and its slot of the frame's sums + 1 (0: none)
static const uint kGiWorldKey = 0u;
static const uint kGiWorldStamp = 1u;
static const uint kGiWorldCount = 2u;
static const uint kGiWorldFaces = 4u;
static const uint kGiWorldSlot = 13u;
// a slot's (GI_WORLD_SLOT_WORD_COUNT): face f's color c's sum at f * 3 + c, then the samples, then its cell + 1
static const uint kGiWorldSlotSamples = 18u;
static const uint kGiWorldSlotCell = 19u;
// the cells a search probes, linearly, before it takes back the stalest of them not added to this frame (the table is a
// policy, the cells least lately used given up, not a capacity: linear probing clusters, and 8 cells in a row were in
// use in Sponza's corridor with a million cells, a sample in 900 frames)
static const uint kGiWorldProbeCount = 16u;
// the sums' fixed point: a sample's value at the frame's exposure (PushConstants::exposure, about what the screen shows
// as white) in steps of 1 / kGiWorldFixedScale, at most kGiWorldSampleMax of them (a thousand times white: what the
// cache's rays meet is bounced light; the emitters' reaches the cache through their samples, not its rays' hits), so
// that GI_WORLD_SLOT_SAMPLE_COUNT samples a frame fill 32 bits at most
static const float kGiWorldFixedScale = 1024.0;
static const float kGiWorldSampleMax = 1048576.0;
// the samples a cell holds before a lookup takes it rather than the coarser level's (the debug view's, GiWorldIrradiance)
static const uint kGiWorldShownSampleCount = 16u;

uint GiWorldCellWord(uint cell, uint word)
{
	return GI_WORLD_COUNTER_COUNT + cell * GI_WORLD_CELL_WORD_COUNT + word;
}

uint GiWorldSlotWord(uint slot, uint word)
{
	return GI_WORLD_COUNTER_COUNT + gGi[gPushConstants.frameIndex][0].worldCellCount * GI_WORLD_CELL_WORD_COUNT + slot * GI_WORLD_SLOT_WORD_COUNT + word;
}

uint GiWorldMix(uint h)
{
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	return h;
}

// a surface's normal bucket: the axis it faces most, and which way (0 to 5); 6 for a point in the air (no normal)
uint GiWorldBucket(float3 normal)
{
	if (dot(normal, normal) < 0.5)
		return 6u;
	float3 extent = abs(normal);
	uint axis = extent.x >= extent.y && extent.x >= extent.z ? 0u : (extent.y >= extent.z ? 1u : 2u);
	return axis * 2u + (normal[axis] < 0.0 ? 1u : 0u);
}

int3 GiWorldCellOf(float3 position, uint level)
{
	float side = gGi[gPushConstants.frameIndex][0].worldCellSide * float(1u << level);
	return int3(floor(position / side));
}

// a cell's index in the table, ~0 where there is none: found by its key along the probe from its hash, or made (insert)
// in the first empty one, or in the stalest probed if none is empty and that wasn't added to this frame (emptied: its
// mean was another place's, GI_WORLD_COUNTER_CELLS_TAKEN); none, counted (GI_WORLD_COUNTER_NO_CELL: the sample isn't
// added at that level), where every one probed was added to this frame
uint GiWorldFind(int3 place, uint level, uint bucket, bool insert)
{
	uint frameIndex = gPushConstants.frameIndex;
	uint capacity = gGi[frameIndex][0].worldCellCount;
	uint frame = gGi[frameIndex][0].worldFrame;
	uint hash = GiWorldMix(uint(place.x) * 73856093u ^ GiWorldMix(uint(place.y) * 19349663u ^ GiWorldMix(uint(place.z) * 83492791u ^ (level << 3) ^ bucket)));
	uint key = GiWorldMix(hash ^ 0x9E3779B9u) | 1u;
	uint stalest = ~0u;
	uint stalestAge = 0u;
	for (uint probeIt = 0; probeIt < kGiWorldProbeCount; probeIt++)
	{
		uint index = (hash + probeIt) & (capacity - 1u);
		uint keyWord = GiWorldCellWord(index, kGiWorldKey);
		uint found = gGiWorld[keyWord];
		if (found == key)
			return index;
		if (found == 0u)
		{
			if (!insert)
				return ~0u;
			uint previous;
			InterlockedCompareExchange(gGiWorld[keyWord], 0u, key, previous);
			if (previous == 0u)
			{
				InterlockedAdd(gGiWorld[GI_WORLD_COUNTER_CELLS_MADE], 1u);
				return index;
			}
			if (previous == key)
				return index;
			continue;
		}
		uint age = frame - gGiWorld[GiWorldCellWord(index, kGiWorldStamp)];
		if (age > stalestAge)
		{
			stalestAge = age;
			stalest = index;
		}
	}
	if (!insert)
		return ~0u;
	if (stalest != ~0u && stalestAge > 0u)
	{
		uint keyWord = GiWorldCellWord(stalest, kGiWorldKey);
		uint old = gGiWorld[keyWord];
		uint previous;
		InterlockedCompareExchange(gGiWorld[keyWord], old, key, previous);
		if (previous == old)
		{
			gGiWorld[GiWorldCellWord(stalest, kGiWorldStamp)] = frame;
			gGiWorld[GiWorldCellWord(stalest, kGiWorldCount)] = 0u;
			for (uint wordIt = 0; wordIt < 9u; wordIt++)
				gGiWorld[GiWorldCellWord(stalest, kGiWorldFaces + wordIt)] = 0u;
			InterlockedAdd(gGiWorld[GI_WORLD_COUNTER_CELLS_TAKEN], 1u);
			return stalest;
		}
		if (previous == key)
			return stalest;
	}
	InterlockedAdd(gGiWorld[GI_WORLD_COUNTER_NO_CELL], 1u);
	return ~0u;
}

// a sample of the surface cache's rays (GiCacheUpdate) added to the world cache at every level from firstLevel up (the
// clip's level its ray's voxel is of: a cell no finer than the voxel. every clip's rays at every level, the coarse clips'
// rays, from over the whole scene, made cells of the finest level everywhere: in Sponza's corridor 609 cells made and
// 6131 taken back a frame, still, with a million): from position (on the surface its rays start from), facing normal,
// along direction, its radiance and what it stands for (estimate, as the entry's faces take it: radiance times the
// face's basis times 4 times estimate). random: a number of the ray's own
void GiWorldAdd(float3 position, float3 normal, float3 direction, float3 radiance, float estimate, uint random, uint firstLevel)
{
	uint frameIndex = gPushConstants.frameIndex;
	if (gGi[frameIndex][0].worldOn == 0u)
		return;
	float exposure = gPushConstants.exposure > 0.0 && !isinf(gPushConstants.exposure) ? gPushConstants.exposure : 1.0;
	float scale = exposure * kGiWorldFixedScale;
	float basis[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	GiIrradianceBasis(direction, basis);
	// its value per channel in fixed point, the same at every level, rounded down or up at random by the fraction (so
	// that a sum of many is their sum: rounded down, the dim samples' fractions were lost, each the same way)
	uint values[3u * SHADER_TYPES_GI_IRRADIANCE_COUNT];
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		for (uint colorIt = 0; colorIt < 3u; colorIt++)
		{
			float value = min(radiance[colorIt] * basis[faceIt] * 4.0 * estimate * scale, kGiWorldSampleMax);
			uint channel = faceIt * 3u + colorIt;
			if (!(value > 0.0))
			{
				values[channel] = 0u;
				continue;
			}
			float whole = floor(value);
			float threshold = float(GiWorldMix(random + channel * 0x9E3779B9u) & 0xFFFFFFu) / 16777216.0;
			values[channel] = uint(whole) + (value - whole > threshold ? 1u : 0u);
		}
	InterlockedAdd(gGiWorld[GI_WORLD_COUNTER_SAMPLES], 1u);
	uint bucket = GiWorldBucket(normal);
	uint frame = gGi[frameIndex][0].worldFrame;
	uint slotCount = gGi[frameIndex][0].worldSlotCount;
	for (uint levelIt = firstLevel; levelIt < gGi[frameIndex][0].worldLevelCount; levelIt++)
	{
		uint cell = GiWorldFind(GiWorldCellOf(position, levelIt), levelIt, bucket, true);
		if (cell == ~0u)
			continue;
		gGiWorld[GiWorldCellWord(cell, kGiWorldStamp)] = frame;
		// the cell's slot of the frame's sums: taken by the first sample of the frame (a slot counted, then claimed by
		// a compare and exchange; one that lost the race is left empty, which the fold skips)
		uint slotWord = GiWorldCellWord(cell, kGiWorldSlot);
		uint slot = gGiWorld[slotWord];
		if (slot == 0u)
		{
			uint taken;
			InterlockedAdd(gGiWorld[GI_WORLD_COUNTER_SLOTS], 1u, taken);
			if (taken >= slotCount)
			{
				InterlockedAdd(gGiWorld[GI_WORLD_COUNTER_NO_SLOT], 1u);
				continue;
			}
			uint previous;
			InterlockedCompareExchange(gGiWorld[slotWord], 0u, taken + 1u, previous);
			if (previous == 0u)
			{
				gGiWorld[GiWorldSlotWord(taken, kGiWorldSlotCell)] = cell + 1u;
				slot = taken + 1u;
			}
			else
				slot = previous;
		}
		uint before;
		InterlockedAdd(gGiWorld[GiWorldSlotWord(slot - 1u, kGiWorldSlotSamples)], 1u, before);
		if (before >= GI_WORLD_SLOT_SAMPLE_COUNT)
		{
			InterlockedAdd(gGiWorld[GI_WORLD_COUNTER_SLOT_FULL], 1u);
			continue;
		}
		for (uint channelIt = 0; channelIt < 3u * SHADER_TYPES_GI_IRRADIANCE_COUNT; channelIt++)
			if (values[channelIt] != 0u)
				InterlockedAdd(gGiWorld[GiWorldSlotWord(slot - 1u, channelIt)], values[channelIt]);
	}
}

// a cell's faces (see kGiWorldFaces)
void GiWorldLoadFaces(uint cell, out float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT])
{
	uint4 words[3];
	for (uint wordIt = 0; wordIt < 12u; wordIt++)
		words[wordIt / 4u][wordIt % 4u] = wordIt < 9u ? gGiWorld[GiWorldCellWord(cell, kGiWorldFaces + wordIt)] : 0u;
	GiCacheUnpackFaces(words, faces);
}

// the frame's sums folded into their cells' running means, a thread a slot of those taken (GiWorldAdd): each sum over
// the samples is the frame's mean, which moves the cell's by the samples' share of its window (GiData::worldWindow;
// all of it where the frame had more), its faces stored rounded at random (GiHalfDithered: rounded to the nearest, a
// long window's means climbed); the slot emptied for the next frame. and the frame's counters copied for the cpu
// (gGiStatistics, GI_STATISTIC_WORLD_FIRST)
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiWorldFold(uint3 dispatchId: SV_DispatchThreadID)
{
	uint slot = GiThread(dispatchId).x;
	uint frameIndex = gPushConstants.frameIndex;
	if (slot == 0u)
		for (uint counterIt = 0; counterIt < GI_WORLD_COUNTER_COUNT; counterIt++)
			gGiStatistics[frameIndex][GI_STATISTIC_WORLD_FIRST + counterIt] = gGiWorld[counterIt];
	uint slotCount = gGi[frameIndex][0].worldSlotCount;
	if (slot >= min(gGiWorld[GI_WORLD_COUNTER_SLOTS], slotCount))
		return;
	uint cellPlus = gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotCell)];
	uint sampleCount = min(gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotSamples)], GI_WORLD_SLOT_SAMPLE_COUNT);
	if (cellPlus != 0u && sampleCount != 0u)
	{
		uint cell = cellPlus - 1u;
		float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT];
		GiWorldLoadFaces(cell, faces);
		uint count = gGi[frameIndex][0].worldOn != 0u ? gGiWorld[GiWorldCellWord(cell, kGiWorldCount)] : 0u;
		uint window = max(gGi[frameIndex][0].worldWindow, 1u);
		uint total = min(count + sampleCount, window);
		float share = min(float(sampleCount) / float(max(total, 1u)), 1.0);
		float exposure = gPushConstants.exposure > 0.0 && !isinf(gPushConstants.exposure) ? gPushConstants.exposure : 1.0;
		float unit = 1.0 / (float(sampleCount) * exposure * kGiWorldFixedScale);
		for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		{
			float3 mean = float3(
				float(gGiWorld[GiWorldSlotWord(slot, faceIt * 3u)]), float(gGiWorld[GiWorldSlotWord(slot, faceIt * 3u + 1u)]),
				float(gGiWorld[GiWorldSlotWord(slot, faceIt * 3u + 2u)])) * unit;
			faces[faceIt] += (mean - faces[faceIt]) * share;
		}
		uint packed[9];
		GiCachePackFaces(faces, packed, (cell * 0x9E3779B1u ^ gGi[frameIndex][0].worldFrame * 0x85EBCA6Bu) | 1u);
		for (uint wordIt = 0; wordIt < 9u; wordIt++)
			gGiWorld[GiWorldCellWord(cell, kGiWorldFaces + wordIt)] = packed[wordIt];
		gGiWorld[GiWorldCellWord(cell, kGiWorldCount)] = total;
		gGiWorld[GiWorldCellWord(cell, kGiWorldSlot)] = 0u;
	}
	for (uint wordIt = 0; wordIt < GI_WORLD_SLOT_WORD_COUNT; wordIt++)
		gGiWorld[GiWorldSlotWord(slot, wordIt)] = 0u;
}

// the world cache's light about a point, as a white surface facing normal reflects it (as GiEvaluate's), for the debug
// view (DEBUG_VIEW_GI_WORLD): from the level whose cell is about a 64th of the distance from the eye, or the first
// coarser one holding kGiWorldShownSampleCount samples (the last whatever it holds); -1 where none holds any
float3 GiWorldIrradiance(float3 position, float3 normal, float distance)
{
	uint frameIndex = gPushConstants.frameIndex;
	if (gGi[frameIndex][0].worldOn == 0u)
		return -1.0;
	float side = gGi[frameIndex][0].worldCellSide;
	uint levelCount = gGi[frameIndex][0].worldLevelCount;
	uint start = uint(clamp(floor(log2(max(distance / 64.0, side) / side)), 0.0, float(levelCount - 1u)));
	uint bucket = GiWorldBucket(normal);
	for (uint levelIt = start; levelIt < levelCount; levelIt++)
	{
		uint cell = GiWorldFind(GiWorldCellOf(position, levelIt), levelIt, bucket, false);
		if (cell == ~0u)
			continue;
		uint count = gGiWorld[GiWorldCellWord(cell, kGiWorldCount)];
		if (count == 0u || (count < kGiWorldShownSampleCount && levelIt + 1u < levelCount))
			continue;
		float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT];
		GiWorldLoadFaces(cell, faces);
		float3 w = normal * normal;
		float3 x = normal.x >= 0.0 ? faces[0] : faces[1];
		float3 y = normal.y >= 0.0 ? faces[2] : faces[3];
		float3 z = normal.z >= 0.0 ? faces[4] : faces[5];
		return max(w.x * x + w.y * y + w.z * z, 0.0);
	}
	return -1.0;
}
