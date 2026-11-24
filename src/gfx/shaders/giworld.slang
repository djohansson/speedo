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
// samples its mean is of, the samples its filtered light stands for (see ComputeGiWorldFilter), its faces as halves (9
// words, as a cache entry's: an ambient cube's light along the axes, GiCachePackFaces), its slot of the frame's sums + 1
// (0: none), the samples its second filter pass's faces stand for, a spare, its filtered faces (9 words, the last pass's,
// which the lookups read), its second pass's (9 words: the passes take turns, see ComputeGiWorldFilter) and where its
// samples lie (kGiWorldWhere, 3 words)
static const uint kGiWorldKey = 0u;
static const uint kGiWorldStamp = 1u;
static const uint kGiWorldCount = 2u;
static const uint kGiWorldFilteredCount = 3u;
static const uint kGiWorldFaces = 4u;
static const uint kGiWorldSlot = 13u;
static const uint kGiWorldFiltered = 16u;
static const uint kGiWorldPassCount = 14u;
static const uint kGiWorldPass = 25u;
// (the spare word after kGiWorldPassCount: the mean of its samples' squares, a float, of the light along its bucket's face,
// for the filter's edge test, as the surface cache's entries keep theirs)
static const uint kGiWorldMoment = 15u;
// where its samples lie in the cell: their mean position from its corner over its side (0 to 1 each way), 3 floats each
// plus 1 (0: none yet; GiWorldWhereOf). along its bucket's axis that is where the surface they are of lies (its depth),
// where the cell's place is only where the level's grid cuts the world: a lookup takes the two cells across the surface
// by how near their samples lie to the point (GiWorldLevelFaces), where the trilinear's weights took the cells of other
// surfaces facing the same way a layer off (MovingInterior's floor the ground's outside, under the sky). and along the
// surface it is where a lookup marches to, to tell whether the cell's light is of its side of what is between
// (GiWorldReach)
static const uint kGiWorldWhere = 34u;
// a slot's (GI_WORLD_SLOT_WORD_COUNT): face f's color c's sum at f * 3 + c, then the samples, its cell + 1, and the cell's
// place (3 words, ints) and its level and bucket (level << 8 | bucket), for the filter to find its neighbours
static const uint kGiWorldSlotSamples = 18u;
static const uint kGiWorldSlotCell = 19u;
static const uint kGiWorldSlotPlace = 20u;
static const uint kGiWorldSlotLevel = 23u;
// and the sum of its samples' squares (the light along the bucket's face, in fixed point, squared, over kGiWorldSquareScale)
static const uint kGiWorldSlotSquares = 24u;
// and the sums of where its samples lie in the cell (3 words, see kGiWorldWhere), in 1 / kGiWorldWhereScale of its side
static const uint kGiWorldSlotWhere = 25u;
static const float kGiWorldWhereScale = 256.0;
static const float kGiWorldSquareScale = 1048576.0;
// the cells a search probes, linearly, before it takes back the stalest of them not added to this frame (the table is a
// policy, the cells least lately used given up, not a capacity: linear probing clusters, and 8 cells in a row were in
// use in Sponza's corridor with a million cells, a sample in 900 frames)
static const uint kGiWorldProbeCount = 16u;
// the rays a surface cache entry has traced before its samples go to the world cache (see GiCacheUpdate's toWorld)
static const uint kGiWorldFromRayCount = 16u;
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

// where a cell's probe begins (its hash) and the key it is told apart by: two hashes of its place, level and bucket,
// independent of each other. the key was the hash's own mix, so two places of the same hash (32 bits: about a hundred
// pairs among a million cells) had the same key too and were one cell, of both places' samples: a cell of a sunlit
// place's light here and there in the shade (the samples a cell under Sponza's vault took averaged 4e-5 along its face,
// its sums 0.0177: bright diamonds on the vaults and walls, the user's report, 2026-11-19). now two places are one cell
// only where both hashes agree and their probes meet: about one pair in a hundred tables
uint GiWorldHash(int3 place, uint level, uint bucket)
{
	return GiWorldMix(uint(place.x) * 73856093u ^ GiWorldMix(uint(place.y) * 19349663u ^ GiWorldMix(uint(place.z) * 83492791u ^ (level << 3) ^ bucket)));
}

uint GiWorldKey(int3 place, uint level, uint bucket)
{
	return GiWorldMix(uint(place.z) * 0x632BE59Bu ^ GiWorldMix(uint(place.x) * 0x85157AF5u ^ GiWorldMix(uint(place.y) * 0x9E3779B1u ^ (bucket << 5) ^ level ^ 0xA511E9B3u))) | 1u;
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

// where a position lies in its cell of a level (0 to 1 of the cell's side each way, see kGiWorldWhere)
float3 GiWorldWhereIn(float3 position, uint level)
{
	float3 along = position / (gGi[gPushConstants.frameIndex][0].worldCellSide * float(1u << level));
	return along - floor(along);
}

// where a cell's samples lie (kGiWorldWhere), -2 where it has none yet
float3 GiWorldWhereOf(uint cell)
{
	uint first = gGiWorld[GiWorldCellWord(cell, kGiWorldWhere)];
	if (first == 0u)
		return -2.0;
	return float3(asfloat(first), asfloat(gGiWorld[GiWorldCellWord(cell, kGiWorldWhere + 1u)]), asfloat(gGiWorld[GiWorldCellWord(cell, kGiWorldWhere + 2u)])) - 1.0;
}

// the samples a cell new to the table takes its parent's light as (the cell of the next level up holding it, of its
// bucket; GiWorldSeed): new, a cell held the few samples of its first frames alone, one that met a bright surface all of
// its light, and walking along Sponza's corridor the floor ahead was strewn with bright cells (moving against settled
// 2.75 to 3.95 of 255 through the grid's own view, where settled it is smooth): as the clipmap's cache began an entry
// new to a clip from the coarser clip's (no longer, see GiCacheUpdate)
static const uint kGiWorldSeedCount = 64u;

// a cell new to the table begun from its parent's light (see kGiWorldSeedCount), where its parent holds any (the samples
// are added coarse first, GiWorldAdd, so a sample's parent cell is there before its child)
void GiWorldSeed(uint cell, int3 place, uint level, uint bucket)
{
	uint frameIndex = gPushConstants.frameIndex;
	if (level + 1u >= gGi[frameIndex][0].worldLevelCount)
		return;
	// (found along its probe by its key alone: GiWorldFind, which seeds, can't be called from here, recursion)
	int3 parentPlace = place >> 1;
	uint parentLevel = level + 1u;
	uint hash = GiWorldHash(parentPlace, parentLevel, bucket);
	uint key = GiWorldKey(parentPlace, parentLevel, bucket);
	uint capacity = gGi[frameIndex][0].worldCellCount;
	uint parent = ~0u;
	for (uint probeIt = 0; probeIt < kGiWorldProbeCount && parent == ~0u; probeIt++)
	{
		uint index = (hash + probeIt) & (capacity - 1u);
		uint found = gGiWorld[GiWorldCellWord(index, kGiWorldKey)];
		if (found == 0u)
			break;
		if (found == key)
			parent = index;
	}
	if (parent == ~0u)
		return;
	uint count = gGiWorld[GiWorldCellWord(parent, kGiWorldCount)];
	if (count == 0u)
		return;
	for (uint wordIt = 0; wordIt < 9u; wordIt++)
	{
		gGiWorld[GiWorldCellWord(cell, kGiWorldFaces + wordIt)] = gGiWorld[GiWorldCellWord(parent, kGiWorldFaces + wordIt)];
		gGiWorld[GiWorldCellWord(cell, kGiWorldFiltered + wordIt)] = gGiWorld[GiWorldCellWord(parent, kGiWorldFiltered + wordIt)];
	}
	gGiWorld[GiWorldCellWord(cell, kGiWorldCount)] = min(count, kGiWorldSeedCount);
	gGiWorld[GiWorldCellWord(cell, kGiWorldMoment)] = gGiWorld[GiWorldCellWord(parent, kGiWorldMoment)];
	gGiWorld[GiWorldCellWord(cell, kGiWorldFilteredCount)] = min(gGiWorld[GiWorldCellWord(parent, kGiWorldFilteredCount)], kGiWorldSeedCount);
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
	uint hash = GiWorldHash(place, level, bucket);
	uint key = GiWorldKey(place, level, bucket);
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
				GiWorldSeed(index, place, level, bucket);
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
			gGiWorld[GiWorldCellWord(stalest, kGiWorldFilteredCount)] = 0u;
			gGiWorld[GiWorldCellWord(stalest, kGiWorldPassCount)] = 0u;
			gGiWorld[GiWorldCellWord(stalest, kGiWorldMoment)] = 0u;
			for (uint axisIt = 0; axisIt < 3u; axisIt++)
				gGiWorld[GiWorldCellWord(stalest, kGiWorldWhere + axisIt)] = 0u;
			for (uint wordIt = 0; wordIt < 9u; wordIt++)
				gGiWorld[GiWorldCellWord(stalest, kGiWorldFaces + wordIt)] = 0u;
			InterlockedAdd(gGiWorld[GI_WORLD_COUNTER_CELLS_TAKEN], 1u);
			GiWorldSeed(stalest, place, level, bucket);
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
	if (Skipped(SHADER_TYPES_LAYERS_WORLD) || gGi[frameIndex][0].worldOn == 0u)
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
	// its light along the bucket's face, squared (for the cells' variance: kGiWorldMoment), rounded at random as the values
	uint square = 0u;
	{
		uint face = min(bucket, SHADER_TYPES_GI_IRRADIANCE_COUNT - 1u);
		float light = dot(float3(values[face * 3u], values[face * 3u + 1u], values[face * 3u + 2u]), kGiCacheLuminance);
		float scaled = min(light * light / kGiWorldSquareScale, kGiWorldSampleMax);
		float whole = floor(scaled);
		float threshold = float(GiWorldMix(random ^ 0x51ed270bu) & 0xFFFFFFu) / 16777216.0;
		square = uint(whole) + (scaled - whole > threshold ? 1u : 0u);
	}
	uint frame = gGi[frameIndex][0].worldFrame;
	uint slotCount = gGi[frameIndex][0].worldSlotCount;
	// (coarse first: a cell new to the table begins from its parent's light, GiWorldSeed)
	for (uint levelIt = gGi[frameIndex][0].worldLevelCount - 1u; levelIt + 1u > firstLevel; levelIt--)
	{
		int3 place = GiWorldCellOf(position, levelIt);
		float3 where = GiWorldWhereIn(position, levelIt);
		uint cell = GiWorldFind(place, levelIt, bucket, true);
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
				for (uint axisIt = 0; axisIt < 3u; axisIt++)
					gGiWorld[GiWorldSlotWord(taken, kGiWorldSlotPlace + axisIt)] = asuint(place[axisIt]);
				gGiWorld[GiWorldSlotWord(taken, kGiWorldSlotLevel)] = (levelIt << 8) | bucket;
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
		if (square != 0u)
			InterlockedAdd(gGiWorld[GiWorldSlotWord(slot - 1u, kGiWorldSlotSquares)], square);
		for (uint axisIt = 0; axisIt < 3u; axisIt++)
			InterlockedAdd(gGiWorld[GiWorldSlotWord(slot - 1u, kGiWorldSlotWhere + axisIt)], uint(round(where[axisIt] * kGiWorldWhereScale)));
	}
}

// a cell's faces (see kGiWorldFaces; first: kGiWorldFiltered for its filtered ones)
void GiWorldLoadFaces(uint cell, out float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT], uint first = kGiWorldFaces)
{
	uint4 words[3];
	for (uint wordIt = 0; wordIt < 12u; wordIt++)
		words[wordIt / 4u][wordIt % 4u] = wordIt < 9u ? gGiWorld[GiWorldCellWord(cell, first + wordIt)] : 0u;
	GiCacheUnpackFaces(words, faces);
}

// the frame's sums folded into their cells' running means, a thread a slot of those taken (GiWorldAdd): each sum over
// the samples is the frame's mean, which moves the cell's by the samples' share of its window (GiData::worldWindow;
// all of it where the frame had more), its faces stored rounded at random (GiHalfDithered: rounded to the nearest, a
// long window's means climbed); the slot is kept for the filter (ComputeGiWorldFilter), which empties it. and the
// frame's counters copied for the cpu (gGiStatistics, GI_STATISTIC_WORLD_FIRST)
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
	// and the table's occupancy (see GI_STATISTIC_WORLD_HELD): a 32nd of the cells, a thread each, counted a wave at a
	// time
	{
		uint share = gGi[frameIndex][0].worldCellCount / GI_WORLD_OCCUPANCY_SWEEP_COUNT;
		uint frame = gGi[frameIndex][0].worldFrame;
		bool held = false;
		bool live = false;
		if (slot < share)
		{
			uint cell = (frame % GI_WORLD_OCCUPANCY_SWEEP_COUNT) * share + slot;
			held = gGiWorld[GiWorldCellWord(cell, kGiWorldKey)] != 0u;
			live = held && float(frame - gGiWorld[GiWorldCellWord(cell, kGiWorldStamp)]) < kGiWorldStaleReach;
		}
		uint heldCount = WaveActiveCountBits(held);
		uint liveCount = WaveActiveCountBits(live);
		if (WaveGetLaneIndex() == WaveActiveMin(WaveGetLaneIndex()) && heldCount != 0u)
		{
			InterlockedAdd(gGiStatistics[frameIndex][GI_STATISTIC_WORLD_HELD], heldCount);
			InterlockedAdd(gGiStatistics[frameIndex][GI_STATISTIC_WORLD_LIVE], liveCount);
		}
	}
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
		// (and the mean of the squares, likewise: the sum's in the fixed point's units squared, see kGiWorldSlotSquares)
		float meanSquare = float(gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotSquares)]) * kGiWorldSquareScale * unit / (exposure * kGiWorldFixedScale);
		float moment = asfloat(gGiWorld[GiWorldCellWord(cell, kGiWorldMoment)]);
		moment = isnan(moment) || isinf(moment) ? 0.0 : moment;
		gGiWorld[GiWorldCellWord(cell, kGiWorldMoment)] = asuint(moment + (meanSquare - moment) * share);
		// (and where its samples lie, likewise; its first frame's where it had none: its seed is its parent's light, not
		// where its samples are)
		float3 frameWhere = float3(gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotWhere)], gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotWhere + 1u)],
								   gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotWhere + 2u)]) / (float(sampleCount) * kGiWorldWhereScale);
		float3 where = GiWorldWhereOf(cell);
		where = where.x < -1.0 ? frameWhere : where + (frameWhere - where) * share;
		for (uint axisIt = 0; axisIt < 3u; axisIt++)
			gGiWorld[GiWorldCellWord(cell, kGiWorldWhere + axisIt)] = asuint(saturate(where[axisIt]) + 1.0);
	}
}

// how many frames since a cell was last added to it keeps all of its weight, in the lookups and as a filter's tap, and
// at how many none (see GiWorldLevelFaces, ComputeGiWorldFilter): a cell no longer added to is of what isn't traced
// now, out of view or behind something, and holds what was, sure of it. after a flight through Sponza (the user's,
// 2026-11-19), the top of a capital under the arcade's vault took the sky's light of cells the camera had passed in the
// courtyard (bluish white; the lookup's ledge 20.4 of 255 against the clipmap's 14.2, 13.9 with the old cells left
// out), and the cells of the walls under the vaults the light of the walls' cells over the vaults, of the sunlit
// gallery, through the filter's taps (1, 2 and 4 cells apart along the surface, past what is between: a cell's mean
// 6.8e-5 and 7.9e-5 after a tour of the model and fresh, its filtered light 3.6e-4 and 8.6e-5), a glow along the walls'
// tops that grew with the places visited before. an entry in use traces a ray every frame or two, and one in a block
// no lookup reads every 8 (kGiCacheIdleFrameCount), settled half as often
static const float kGiWorldStaleNear = 64.0;
static const float kGiWorldStaleReach = 192.0;

// the edge test of the filter's taps: how many deviations of the two means' noise apart their light may be and still be
// taken (as the surface cache's filter's, GiData::cacheSigma), each mean's variance its samples' (kGiWorldMoment less its
// mean's square) over the samples it stands for. by its light times 2 over the root of its samples, a cell that caught a
// rare bright sample (the sun's band) stood apart from its neighbours as an edge, and kept it: bright spots on Sponza's
// vault and walls, blue ones in the floor's band
static const float kGiWorldEdgeSigma = 2.0;

// a cell's samples' variance of the light along its bucket's face (see kGiWorldMoment)
float GiWorldVariance(uint cell, uint face)
{
	// (the face's three halves alone, of the two words that hold them: the 9 of every face, a tap's twice over, were most
	// of the filter's reads)
	uint half = face * 3u;
	uint first = gGiWorld[GiWorldCellWord(cell, kGiWorldFaces + half / 2u)];
	uint second = gGiWorld[GiWorldCellWord(cell, kGiWorldFaces + half / 2u + 1u)];
	float3 color = (half & 1u) == 0u ? float3(f16tof32(first & 0xFFFFu), f16tof32(first >> 16), f16tof32(second & 0xFFFFu))
									 : float3(f16tof32(first >> 16), f16tof32(second & 0xFFFFu), f16tof32(second >> 16));
	float light = dot(color, kGiCacheLuminance);
	float moment = asfloat(gGiWorld[GiWorldCellWord(cell, kGiWorldMoment)]);
	return isnan(moment) || isinf(moment) ? 0.0 : max(moment - light * light, 0.0);
}

// the passes of the filter (GiData's step, PushConstants::giStep: 0 to kGiWorldFilterPassCount - 1), a-trous as the surface
// cache's (ComputeGiCacheFilter): its taps 1, 2, 4 cells apart, so that three passes reach 7 cells each way
static const uint kGiWorldFilterPassCount = 3u;

// the cells the frame added to filtered (after GI World Fold's folds, a thread a slot; step 2 of the plan), a pass at a
// time: each the mean of its 26 neighbours at its level and of its bucket, the pass's step apart, and itself, each by its
// place (1 itself, a half beside, a quarter along an edge, an eighth at a corner), by how sure it is (its samples over
// kGiWorldShownSampleCount, at most 1) and by the edge test (kGiWorldEdgeSigma, each tap's noise by the samples it stands
// for), from the pass before's faces (the means, then kGiWorldFiltered, then kGiWorldPass) into the next's (kGiWorldFiltered,
// kGiWorldPass, kGiWorldFiltered: the last the lookups read) with the samples those stand for (the taps' by their
// weights). unfiltered, the cells' light was mottled where the clipmap's, its entries filtered over about 7 voxels each way,
// was smooth (the cells hold only the rays that start in them); filtered over their 26 neighbours alone, the walk along
// Sponza's corridor differed from its settled frames by each run's own blotches (2.83 to 3.56 of 255 through the grid's
// view). after the last pass the slot and the cell's mark of it are emptied for the next frame
[shader("compute")]
[numthreads(SHADER_TYPES_GI_GROUP_SIZE, 1, 1)]
void ComputeGiWorldFilter(uint3 dispatchId: SV_DispatchThreadID)
{
	uint slot = GiThread(dispatchId).x;
	uint frameIndex = gPushConstants.frameIndex;
	uint slotCount = gGi[frameIndex][0].worldSlotCount;
	if (slot >= min(gGiWorld[GI_WORLD_COUNTER_SLOTS], slotCount))
		return;
	uint pass = gPushConstants.giStep;
	bool last = pass + 1u >= kGiWorldFilterPassCount;
	uint from = pass == 0u ? kGiWorldFaces : (pass % 2u == 1u ? kGiWorldFiltered : kGiWorldPass);
	uint fromCount = pass == 0u ? kGiWorldCount : (pass % 2u == 1u ? kGiWorldFilteredCount : kGiWorldPassCount);
	uint to = pass % 2u == 0u ? kGiWorldFiltered : kGiWorldPass;
	uint toCount = pass % 2u == 0u ? kGiWorldFilteredCount : kGiWorldPassCount;
	int stride = 1 << pass;
	uint cellPlus = gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotCell)];
	if (cellPlus != 0u)
	{
		uint cell = cellPlus - 1u;
		int3 place = int3(asint(gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotPlace)]), asint(gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotPlace + 1u)]),
						  asint(gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotPlace + 2u)]));
		uint levelBucket = gGiWorld[GiWorldSlotWord(slot, kGiWorldSlotLevel)];
		uint level = levelBucket >> 8;
		uint bucket = levelBucket & 0xFFu;
		float3 own[SHADER_TYPES_GI_IRRADIANCE_COUNT];
		GiWorldLoadFaces(cell, own, from);
		float ownCount = float(gGiWorld[GiWorldCellWord(cell, fromCount)]);
		uint face = min(bucket, SHADER_TYPES_GI_IRRADIANCE_COUNT - 1u);
		float ownLight = dot(own[face], kGiCacheLuminance);
		float ownVariance = GiWorldVariance(cell, face) / max(ownCount, 1.0);
		float3 sum[SHADER_TYPES_GI_IRRADIANCE_COUNT];
		for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
			sum[faceIt] = 0.0;
		float weightSum = 0.0;
		float samples = 0.0;
		for (int z = -1; z <= 1; z++)
			for (int y = -1; y <= 1; y++)
				for (int x = -1; x <= 1; x++)
				{
					// (along the surface alone: the taps across its bucket's axis are of other surfaces facing the same way, and 9
					// taps a pass rather than 27)
					if (int3(x, y, z)[face / 2u] != 0)
						continue;
					bool self = x == 0 && y == 0 && z == 0;
					uint tap = self ? cell : GiWorldFind(place + int3(x, y, z) * stride, level, bucket, false);
					if (tap == ~0u)
						continue;
					float count = self ? ownCount : float(gGiWorld[GiWorldCellWord(tap, fromCount)]);
					if (count <= 0.0)
						continue;
					float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT];
					GiWorldLoadFaces(tap, faces, from);
					float light = dot(faces[face], kGiCacheLuminance);
					float variance = (self ? GiWorldVariance(cell, face) : GiWorldVariance(tap, face)) / count;
					float kernel = (x == 0 ? 1.0 : 0.5) * (y == 0 ? 1.0 : 0.5) * (z == 0 ? 1.0 : 0.5);
					float weight = kernel * min(count / float(kGiWorldShownSampleCount), 1.0) *
								   exp(-abs(light - ownLight) / (kGiWorldEdgeSigma * sqrt(ownVariance + variance) + 1e-12));
					// (and by how lately it was added to, see kGiWorldStaleNear)
					if (!self)
						weight *= saturate((kGiWorldStaleReach - float(gGi[frameIndex][0].worldFrame - gGiWorld[GiWorldCellWord(tap, kGiWorldStamp)])) / (kGiWorldStaleReach - kGiWorldStaleNear));
					for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
						sum[faceIt] += faces[faceIt] * weight;
					weightSum += weight;
					samples += count * weight;
				}
		if (weightSum > 0.0)
		{
			for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
				sum[faceIt] /= weightSum;
			uint packed[9];
			GiCachePackFaces(sum, packed, (cell * 0x85EBCA6Bu ^ gGi[frameIndex][0].worldFrame * 0x9E3779B1u) | 1u);
			for (uint wordIt = 0; wordIt < 9u; wordIt++)
				gGiWorld[GiWorldCellWord(cell, to + wordIt)] = packed[wordIt];
			gGiWorld[GiWorldCellWord(cell, toCount)] = uint(min(samples, 65535.0));
		}
		if (last)
			gGiWorld[GiWorldCellWord(cell, kGiWorldSlot)] = 0u;
	}
	if (last)
		for (uint wordIt = 0; wordIt < GI_WORLD_SLOT_WORD_COUNT; wordIt++)
			gGiWorld[GiWorldSlotWord(slot, wordIt)] = 0u;
}

// the world cache's light about a point, as a white surface facing normal reflects it (as GiEvaluate's), for the debug
// view (DEBUG_VIEW_GI_WORLD): from the level whose cell is about a 64th of the distance from the eye, or the first
// coarser one holding kGiWorldShownSampleCount samples (the last whatever it holds); -1 where none holds any
float3 GiWorldIrradiance(float3 position, float3 normal, float distance)
{
	uint frameIndex = gPushConstants.frameIndex;
	if (Skipped(SHADER_TYPES_LAYERS_WORLD) || gGi[frameIndex][0].worldOn == 0u)
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
		uint count = gGiWorld[GiWorldCellWord(cell, kGiWorldFilteredCount)];
		if (count == 0u || (count < kGiWorldShownSampleCount && levelIt + 1u < levelCount))
			continue;
		float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT];
		GiWorldLoadFaces(cell, faces, kGiWorldFiltered);
		float3 w = normal * normal;
		float3 x = normal.x >= 0.0 ? faces[0] : faces[1];
		float3 y = normal.y >= 0.0 ? faces[2] : faces[3];
		float3 z = normal.z >= 0.0 ? faces[4] : faces[5];
		return max(w.x * x + w.y * y + w.z * z, 0.0);
	}
	return -1.0;
}

// the share of the distance from the eye a lookup's cell is (step 2 of the plan): a twentieth, as the clips' voxels are
// about where each lights (the finest clip's voxel lights up to about 20 of them from the eye)
static const float kGiWorldLookupShare = 1.0 / 20.0;

// how far a lookup's point and its cells' centers are lifted off the surface for GiWorldReach, in the clip's voxels: a
// surface's own voxel is up to 2 in front of it (its center 0.5 to 1.5, see GiVoxelPlane), and a march along the surface
// within those ran into it everywhere
static const float kGiWorldReachLift = 2.0;

// how squarely a voxel's surface faces back along a march for the march to have gone into it, among the voxels it starts
// in (GiWorldReach)
static const float kGiWorldReachFacing = 0.5;

// how much of the way between two points is clear of a clip's voxels (those from is in left out, see below): the share of
// the way to the first voxel met, 1 where none is met before to. a lookup's cell where its samples lie (kGiWorldWhere)
// the point can't reach holds another side's light: MovingInterior's floor by the far wall took the light of the
// floor outside, sunlit and under the sky, in a blue line along the wall's foot (the user's report, 2026-11-19). by the
// share rather than whether it is all clear, so that a cell fades as what is between comes nearer the point: taken or
// left whole, the cells of Sponza's end wall behind the arch before it (which the marches, 2 of clip 2's voxels off the
// wall, met) were left by blocks, as the voxels the marches passed decided
float GiWorldReach(GiClipView clip, float3 from, float3 to)
{
	uint3 size = clip.volumeSize.xyz;
	float3 start = (from - clip.volumeOrigin.xyz) / clip.volumeOrigin.w;
	float3 delta = (to - from) / clip.volumeOrigin.w;
	float end = length(delta);
	if (end <= 1e-6)
		return 1.0;
	float3 direction = delta / end;
	int3 voxel = int3(floor(start));
	int3 stepDirection = int3(select(direction >= 0.0, 1, -1));
	float3 inverse = 1.0 / max(abs(direction), 1e-8);
	float3 next = select(direction >= 0.0, float3(voxel + 1) - start, start - float3(voxel)) * inverse;
	// (until the next boundary is past to, by the distance along the ray; the voxels it starts in left out until it is
	// clear of them, but one whose surface it goes into (facing back along it, kGiWorldReachFacing): by an edge the lifted
	// point is within the other surface's voxels, and the marches along the edge met them, in stripes a cell apart; but
	// by MovingInterior's far wall's foot it was within the wall's, and the marches went through the wall)
	bool clear = !GiInside(voxel, size) || (gGiVoxels[clip.voxelFirst + GiVoxelIndex(uint3(voxel), size)].albedo >> 24) == 0u;
	for (uint stepIt = 0; stepIt < 16u; stepIt++)
	{
		float t = min(next.x, min(next.y, next.z));
		if (t >= end)
			break;
		if (next.x < next.y && next.x < next.z)
		{
			next.x += inverse.x;
			voxel.x += stepDirection.x;
		}
		else if (next.y < next.z)
		{
			next.y += inverse.y;
			voxel.y += stepDirection.y;
		}
		else
		{
			next.z += inverse.z;
			voxel.z += stepDirection.z;
		}
		if (!GiInside(voxel, size))
			return 1.0;
		GiVoxel found = gGiVoxels[clip.voxelFirst + GiVoxelIndex(uint3(voxel), size)];
		if ((found.albedo >> 24) != 0u)
		{
			if (!clear && dot(GiVoxelNormal(found.normal), direction) > -kGiWorldReachFacing)
				continue;
			return t / end;
		}
		clear = true;
	}
	return 1.0;
}

// how near a cell's samples lie to a lookup's point across its surface for all of its weight, and how far for none, in
// its cells (see GiWorldFaces)
static const float kGiWorldDepthNear = 0.25;
static const float kGiWorldDepthReach = 0.75;

// a level's light about a point of a surface facing normal (see GiWorldFaces), added to sum by share: trilinear over its
// 8 cells about the point of the surface's bucket, each by its weight and how sure its mean is (its samples over
// kGiWorldShownSampleCount, at most 1); across the surface (along its bucket's axis) the two cells' weights are how near
// their samples lie to the point (kGiWorldWhere, kGiWorldDepthReach), not how near their centers are (the layers are where
// the grid cuts the world); and by how much of the way to them is clear (GiWorldReach, from lifted, lift off the
// surface). returns how much of the point the level covers (its weights' sum, at most 1), its light the mean of what it
// covers
float GiWorldLevelFaces(float3 position, float3 normal, uint bucket, uint level, GiClipView clip, float3 lifted, float lift, float share,
						inout float3 sum[SHADER_TYPES_GI_IRRADIANCE_COUNT])
{
	float side = gGi[gPushConstants.frameIndex][0].worldCellSide * float(1u << level);
	float3 coordinate = position / side - 0.5;
	int3 base = int3(floor(coordinate));
	float3 fraction = coordinate - float3(base);
	uint axis = min(bucket, 5u) / 2u;
	float3 levelSum[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		levelSum[faceIt] = 0.0;
	float weightSum = 0.0;
	for (uint cornerIt = 0; cornerIt < 8u; cornerIt++)
	{
		int3 corner = int3(cornerIt & 1u, (cornerIt >> 1) & 1u, cornerIt >> 2);
		float3 weights = lerp(1.0 - fraction, fraction, float3(corner));
		weights[axis] = 1.0;
		float weight = weights.x * weights.y * weights.z;
		if (weight <= 0.0)
			continue;
		uint cell = GiWorldFind(base + corner, level, bucket, false);
		if (cell == ~0u)
			continue;
		float3 where = GiWorldWhereOf(cell);
		where = where.x < -1.0 ? 0.5 : where;
		float apart = abs(coordinate[axis] + 0.5 - float(base[axis] + corner[axis]) - where[axis]);
		weight *= saturate((kGiWorldDepthReach - apart) / (kGiWorldDepthReach - kGiWorldDepthNear));
		if (weight <= 0.0)
			continue;
		uint count = gGiWorld[GiWorldCellWord(cell, kGiWorldFilteredCount)];
		weight *= min(float(count) / float(kGiWorldShownSampleCount), 1.0);
		// and by how lately it was added to (see kGiWorldStaleNear)
		weight *= saturate((kGiWorldStaleReach - float(gGi[gPushConstants.frameIndex][0].worldFrame - gGiWorld[GiWorldCellWord(cell, kGiWorldStamp)])) / (kGiWorldStaleReach - kGiWorldStaleNear));
		if (weight <= 0.0)
			continue;
		// (to where its samples lie, lifted as the point is: to its center, the cells along an edge whose centers are past
		// the other surface, which hold this one's strip by the edge, were left in a checker along every edge of
		// MovingInterior's room, by the voxels the marches met; to its point nearest the point, the cells across the far
		// wall's foot, which hold the room's floor by the wall and the sunlit floor outside, were taken, and the blue line
		// was back)
		float3 target = (float3(base + corner) + where) * side;
		weight *= GiWorldReach(clip, lifted, target + normal * (lift - dot(target - position, normal)));
		if (weight <= 0.0)
			continue;
		float3 cellFaces[SHADER_TYPES_GI_IRRADIANCE_COUNT];
		GiWorldLoadFaces(cell, cellFaces, kGiWorldFiltered);
		for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
			levelSum[faceIt] += cellFaces[faceIt] * weight;
		weightSum += weight;
	}
	if (!(weightSum > 1e-4))
		return 0.0;
	float covered = saturate(weightSum);
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		sum[faceIt] += levelSum[faceIt] * (share * covered / weightSum);
	return covered;
}

// the world cache's light about a point of a surface facing normal (its faces, as an entry's), for the shading: at the
// level whose cell is kGiWorldLookupShare of the distance from the eye, blended with the next by the fraction (no level
// is taken at once: nothing snaps), each level's light by how much of the point it covers (GiWorldLevelFaces).
// confidence: how much of the point the two cover, 0 to 1; false where they cover none (the clipmap's light the rest).
// (what a level leaves filled from the next coarser, and so on, as the clipmap's clips fill what the finer ones leave:
// Sponza's corridor blotchier, 1.47 and 3.58 of 255 rather than 1.41 and 3.51, see world-cache-plan.md)
bool GiWorldFaces(float3 position, float3 normal, out float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT], out float confidence)
{
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		faces[faceIt] = 0.0;
	confidence = 0.0;
	uint frameIndex = gPushConstants.frameIndex;
	if (Skipped(SHADER_TYPES_LAYERS_WORLD) || gGi[frameIndex][0].worldOn == 0u)
		return false;
	float side = gGi[frameIndex][0].worldCellSide;
	uint levelCount = gGi[frameIndex][0].worldLevelCount;
	float distance = length(position - gViewData[frameIndex][0].eyePosition.xyz);
	float level = clamp(log2(max(distance * kGiWorldLookupShare, side) / side), 0.0, float(levelCount - 1u));
	uint bucket = GiWorldBucket(normal);
	uint lower = uint(floor(level));
	float upperShare = level - float(lower);
	// the cells' reach is marched in the finest clip holding the point (its voxels about the cells' size, see
	// kGiWorldLookupShare), from the point lifted off its surface (kGiWorldReachLift)
	GiClipView clip = GiClipViewAt(GiFinestClipHolding(position, gGi[frameIndex][0].clipCount - 1u));
	float lift = kGiWorldReachLift * clip.volumeOrigin.w;
	float3 lifted = position + normal * lift;
	for (uint passIt = 0; passIt < 2u; passIt++)
	{
		uint levelIt = min(lower + passIt, levelCount - 1u);
		float share = passIt == 0u ? 1.0 - upperShare : upperShare;
		if (share <= 0.0)
			continue;
		confidence += share * GiWorldLevelFaces(position, normal, bucket, levelIt, clip, lifted, lift, share, faces);
	}
	if (!(confidence > 1e-4))
		return false;
	for (uint faceIt = 0; faceIt < SHADER_TYPES_GI_IRRADIANCE_COUNT; faceIt++)
		faces[faceIt] /= confidence;
	confidence = saturate(confidence);
	return true;
}

// a surface's lookup (GiSampleSurface's) with the world cache's light where it is on (step 2 of the plan: the clipmap's
// light for the rest of what the cells don't cover, see GiWorldFaces; a surface something moving holds keeps the
// clipmap's, whose probes follow it, the cells being of what was there)
GiSample GiWorldBlend(GiSample sample, float3 position, float3 normal)
{
	float3 faces[SHADER_TYPES_GI_IRRADIANCE_COUNT];
	float confidence;
	if (!GiWorldFaces(position, normal, faces, confidence))
		return sample;
	float keep = sample.valid ? 1.0 - confidence : 0.0;
	float take = 1.0 - keep;
	sample.r = sample.r * keep + float4(faces[0].r, faces[1].r, faces[2].r, faces[3].r) * take;
	sample.g = sample.g * keep + float4(faces[0].g, faces[1].g, faces[2].g, faces[3].g) * take;
	sample.b = sample.b * keep + float4(faces[0].b, faces[1].b, faces[2].b, faces[3].b) * take;
	sample.z = sample.z * keep + float4(faces[4].r, faces[5].r, faces[4].g, faces[5].g) * take;
	sample.last.xy = sample.last.xy * keep + float2(faces[4].b, faces[5].b) * take;
	sample.valid = true;
	return sample;
}
