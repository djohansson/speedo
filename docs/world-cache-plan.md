# World cache plan: the indirect lighting's light anchored in the world

The surface cache and the probes keep their light per clip of the clipmap, and the clips disagree about the same place
(by 4 to 5 of 255 on average, more in parts: docs/notes/indirect-lighting.md, "Why the clip levels disagree"). Walking,
the disagreement shows as patches that travel with the camera (Sponza's corridor floor beside the columns: moving
against settled 1.19, 1.52 and 1.65 of 255 at the walk's three poses, 2026-11-19), and a clip's scroll or its weight's
hand-over shows one clip's light for another's. This plan replaces where that light is kept with cells anchored in the
world at several resolutions, each sample added to every level, so that the levels are means of the same samples and
agree by construction.

The user's call, 2026-11-19: try it, **side by side with the clipmap's light** (a switch, the clipmap the default) until
the new is shown better in visual quality, performance and memory; then the user decides the default, and later whether
the old goes. The neural alternative was measured and set aside (cells of means learn in a tenth of the steps at a tenth
of the cost, docs/notes/indirect-lighting.md, "A neural radiance cache").

## The structure

A hash table of cells of fixed capacity (a policy, as the shadow atlas's budget is: the oldest cells evicted, not a
capacity to fail on), open addressing with linear probing:

```
struct GiWorldCell          // 64 bytes
{
    uint  key;              // a 32 bit checksum of (level, cell xyz, normal bucket); 0 empty
    uint  stamp;            // the frame it was last read (eviction, and blocks not read traced less, as now)
    half3 faces[6];         // its light along the axes, an ambient cube, as a cache entry's (36 bytes)
    uint  count;            // the samples its mean is of (16 bits), its rays' sequence (16)
    float moment;           // the mean of its samples' squares (variance: the filter, the cut near what moves)
};
```

- **A cell** is a level L (its side `s0 * 2^L`, s0 from the scene's size as the voxels' is now), the world position
  divided by that side and floored, and **a normal bucket** (a surface's cells by which way it faces, so that the two
  sides of a thin wall are different cells, as the voxels' side bits keep them apart now). **Air cells** have no normal
  bucket: the light at a point in the air, as a probe's.
- **Every sample is added to its cell at every level** (4 to 6), coarse levels the same samples' means over a wider
  footprint.
- **A lookup** takes the finest level whose cell is a few pixels on screen and holds enough samples (the benchmark's 16),
  blended toward the next coarser by distance, continuously: no hand-over at a snap, since nothing snaps.
- **Insertion** by 32 bit compare and exchange on the key (every backend has it; Metal's 64 bit atomics are limited),
  collisions caught by the checksum. 2^21 cells, 128 MB, by default (sized for about 150 thousand fine cells in view at
  1080p, its coarser levels and what is kept off screen, at 70% load).

## What it has to replace

The surface cache (its entries, filter and the volumes the lookups read) for the surfaces, and **the probes' roles**,
which step 3 of the anchored cache measured (docs/notes/indirect-lighting.md, "The probes, resolved where they show");
none of these can simply go:

1. **The fog** reads the probes (`AerialPerspectiveAmbient`): without them it takes the sky's light indoors (Sponza's
   corridor blue with haze).
2. **What moves**: a moving thing's voxels are new every frame and the cache's entries where it moves are capped at 32
   samples; its light is mostly the probes'.
3. **The air's light**: the near field's rays that meet nothing (`GiNearTrace`), what lets light through from behind
   (`GiIrradiance` along the normal's opposite), and the lookups beside a surface's voxel of few samples.
4. **Thin things**: lookups beside window frames, shutters and recesses shallower than a voxel take the air voxels'
   light (leaving the probes out of settled blocks made Bistro's shadowed facade 10 to 20 of 255 darker and blotchy and
   the bathroom's window recess 10 to 27 brighter).

Air cells are the grid's answer to all four; each is a gate of its own (step 4), and until every one passes the
clipmap's probes stay, beside the grid.

## Side by side

`SPEEDO_GI_WORLD` (0, the default: the clipmap as now; 1: the grid's light where its step has it), and a row in View >
Rendering Features > Indirect Lighting. The clipmap stays built and run whatever the switch says until step 5, so that
an A/B is one variable and a capture of either is the same build. The grid's passes run only with the switch on, and its
memory is allocated only then.

## How it is judged (every step)

- **Visual quality**: the regression set (`temp/gi-disocclusion/ab.ps1`: the bathroom framed and at the user's poses,
  Sponza's columns and corridor, Bistro by day and night, CornellBox), Sponza's corridor walk (`walkp.ps1`, moving
  against settled), MovingInterior's to-and-fro walk (`pops.ps1`), the far wall's noise between seeds (`curve.ps1`,
  `wallpattern.ps1`), the bathroom's thin shelves from the front, and the user's eye on each.
- **Performance**: the `Frame` scope and the GI passes' gpu timings at the benchmark poses (docs/performance-plan.md),
  warm pipelines, runs interleaved, serial (never alongside other clients).
- **Memory**: each structure's buffers and images by name, from rhi's allocations (a line in the client's exit report,
  `SPEEDO_GPU_TIMINGS=1`, to be added in step 0), the clipmap's and the grid's side by side.

## The steps

### 0. The baseline

The clipmap's numbers on every measure above, at resolutions 128 and 384: the memory of its voxels, probes, volumes,
cache pages and the filter's records; the GI passes' times; the walk, pops and noise. The memory report added for it.

### 1. The grid, filled and shown, nothing shaded from it

The table, the insertion and the eviction; the surface cache's rays (as they are, from the anchored surfaces) add their
samples to the grid too, at every level; a debug view of the cells (level, samples, light). Gate: its memory and the
insertion's cost (the benchmark measured 0.01 ms for 16 thousand samples at 4 levels, dense), the hit rate and the
collisions at the poses.

### 2. The surfaces shaded from the grid (the go or no-go)

`GiSampleSurface`'s light from the grid where the switch is on; the hits still read the voxels. Gate: the main pass's
time against the volumes' trilinear reads (the hash, its probing and the interpolation by hand: the risk of the whole
plan, estimated 0.3 to 1 ms at 1080p), and the walk against settled, where the gain should show. If the lookups cost
more than the walk gains, the plan stops here and says why.

### 3. The cache's rays from the grid's cells

The cells take the rays themselves (from the surfaces their cells hold, as the anchored entries do), so that the
cache's entries, its filter (or one pass over the cells) and the volumes they write aren't needed where the switch is
on. The hits read the grid's light at the hit (with the shadow ray's direct light, as now), with the single sided
surfaces' and the insides' protections (`GiHitVoxel`'s) carried over as the keys and the normal buckets allow. Gate:
the GI pass's time and the memory without the cache's pages.

### 4. The air cells, one role at a time

Each against the scene that showed it: the fog (Sponza's corridor at the default haze), what moves (MovingInterior's
runner and sphere, the green box), the near field's misses and what lets light through (the bathroom's shelves), the thin
things (Bistro's shadowed facade at `564,170,-2436,0,30`, the bathroom's recess at `-0.8,17.0,-6.5,0,1.2`). Gate: each
as good as the probes', or better, by the measures and the user's eye. The probes stay for a role until its gate passes.

### 5. The user's decision

With every measure side by side: the default, and later whether the clipmap's light (the cache, the volumes, the
probes) goes. What stays either way: the voxels where the rays walk them (`SPEEDO_GI_TRACE=voxels`, a model without a
BVH), which find no surface to anchor at, unless the grid is made to serve them too, a decision of its own.

## Progress

- 2026-11-19: the plan written; nothing built.
- 2026-11-19, **step 0, the baseline**. The memory report: `indirect lighting, memory: ...` in the exit report with
  `SPEEDO_GPU_TIMINGS=1` (`Renderer`: the buffers' sizes and the volumes' packed texels, by structure; what the driver
  pads isn't counted). `temp/gi-disocclusion/baseline.ps1` (settled 1500 frames, 1080p, Vulkan, RTX 4070 Ti SUPER, one
  client at a time; the GI passes summed over those run in most frames, Main Visibility the shading with the lookups):

  | scene, resolution | frame (ms) | GI passes (ms) | Main Visibility (ms) | memory (MB): all, voxels, probes, volumes, surface cache |
  |---|---|---|---|---|
  | the bathroom, 128 | 3.47 | 1.67 | 0.36 | 465: 217, 57, 153, 37 |
  | Sponza's corridor, 128 | 4.20 | 2.01 | 0.46 | 332: 118, 30, 112, 73 |
  | Bistro, 128 | 6.01 | 2.68 | 0.50 | 337: 121, 31, 125, 61 |
  | Bistro at night, 128 | 5.39 | 2.64 | 0.51 | 341: 121, 31, 125, 65 |
  | MovingInterior, 128 | 3.00 | 1.50 | 0.37 | 170: 56, 15, 64, 34 |
  | the bathroom, 384 | 13.67 | 20.27 | 0.38 | 11151: 5457, 1393, 3870, 431 |
  | Sponza's corridor, 384 | 29.53 | 22.54 | 0.51 | 6736: 2662, 675, 2685, 715 |
  | Bistro, 384 | 36.80 | 25.18 | 0.80 | 7161: 2723, 698, 3099, 641 |
  | Bistro at night, 384 | 35.64 | 25.64 | 0.88 | 7165: 2723, 698, 3099, 645 |
  | MovingInterior, 384 | 12.48 | 10.96 | 0.38 | 3155: 1169, 302, 1451, 232 |

  The clipmap's memory is its dense structures, the voxels and the volumes, cubic in the resolution (27 times from 128
  to 384: the bathroom's 11 GB of a 16 GB gpu); the surface cache, paged, is a tenth of it. So the world cache's
  memory gain is in what it can take of the volumes (step 2 onwards) and of the voxels and probes (steps 3 and 4), not
  in the cache it replaces first. The quality baselines, measured the same day: Sponza's corridor walk moving against
  settled 1.19, 1.52 and 1.65 of 255 (`walkp.ps1`); MovingInterior's to-and-fro walk's steps 130000 a million pixels
  over the walk, the worst frame 8800 (`pops.ps1`); its far wall's noise between seeds 0.44 of 255 at frame 20000 with
  the window of 4096 (`curve.ps1`, `wallpattern.ps1`); Bistro's night turn's leading strip 14.5 to 17.9 of 255 in the
  emitters' view, 8.6 to 11.1 lit (`turnp.ps1`).

- 2026-11-19, **step 1, the grid filled and shown** (`giworld.slang`, `SPEEDO_GI_WORLD=1`, `SPEEDO_GI_WORLD_CELLS`,
  `SPEEDO_GI_WORLD_LEVELS`; the `gi-world` debug view; the passes GI World Clear and GI World Fold; the exit report's
  `indirect lighting, world cache:` line). As designed below, with what the first runs changed:
  - **A sample feeds the levels from its clip's up** (its ray's voxel's level: a cell no finer than the voxel). Every
    clip's rays at every level, the coarse clips' rays from over the whole building made cells of the finest level
    everywhere: in Sponza's corridor 609 cells made and 6131 taken back a frame, still, with a million.
  - **The table is a policy**: a search probes 16 cells and takes back the stalest not added to this frame where none is
    free; a level without a cell is counted, not failed (linear probing clusters: 8 in a row were in use in Sponza
    with a million cells, a sample in 900 frames, before the fix above).
  - **Slots**: a quarter of the cells (the busiest frame took 90 thousand at the gate's poses).

  The gate (resolution 128, settled 1500 frames, the grid on against off, one client at a time, the runs of a scene
  back to back):

  | scene | the GI passes, off and on (ms) | samples a frame | cells' slots a frame (most) | cells made a frame |
  |---|---|---|---|---|
  | the bathroom | 1.69, 1.89 | 17600 | 27000 (51600) | 87 |
  | Sponza's corridor | 2.01, 2.40 | 44500 | 68900 (87100) | 229 |
  | Bistro | 2.70, 3.22 | 32500 | 53200 (81300) | 222 |
  | Bistro at night | 2.61, 3.20 | 37600 | 59200 (87300) | 225 |
  | MovingInterior | 1.51, 1.59 | 14300 | 24600 (28200) | 95 |

  84 MB (a million cells of 64 bytes, 262 thousand slots of 80), a third of the cells made by the run's end, none taken
  back, no level without a cell. Adding the samples (5 levels' finds and atomic adds a ray) costs 0.1 to 0.6 ms beside
  the clipmap; what it replaces (the cache's entries and filter, the volumes) is steps 2 and 3. The debug view shows the
  cells unfiltered, blocky and noisy where the finest hold few samples, the light where the clipmap has it (the sky's
  blue along Sponza's curtains, `temp/review/world-step1-sponza.png`).

- 2026-11-19, **step 2 begun, the surfaces shaded from the grid** (`GiWorldFaces`, `GiWorldBlend` in `GiSampleSurface`: the
  level whose cell is a twentieth of the distance, blended with the next by the fraction, trilinear over 8 cells of the
  surface's bucket, each by its samples over 16; the clipmap's light for what the cells don't cover and on what moves).
  The lookups cost Main Visibility 0.38 to 0.62 ms at Sponza's corridor (the plan's estimate 0.3 to 1). But unfiltered,
  the cells' light is mottled where the clipmap's is smooth (`temp/review/world-step2-sheet.png`, the indirect diffuse
  three times brighter: the near wall, the vault, the floor's blue band in patches): a cache entry is filtered over about
  7 voxels each way, edge aware, and a cell holds only the rays that start in it. Tried, not kept:
  - the sample spread along the surface by up to a cell either way at random before its cell is found (a box filter for
    nothing): blotchier and brighter (`temp/gi-disocclusion/worldspread.ps1`: the lower half's mean |pixel - the 33x33's
    mean| 1.55 to 1.65 and 1.87 at 1 and 2 cells, its mean 6.99 to 7.67 and 8.64 of 255): spread in cells, the coarse
    levels' spread a metre and more, the curtains' light across the floor;
  - the level by the samples (coarser by half the log2 of how far under 2048 the distance's level's cell is): the
    blotches larger and softer, still there (1.56; at 121 pixels 3.84 where the clipmap is 3.43), the image 7% brighter
    (the coarse cells' footprints take other surfaces of the same facing).
  So step 2 needs a filter of the cells' own, edge aware as the cache's:
  - **the cells filtered** (`ComputeGiWorldFilter`, in GI World Fold's pass after the folds: each cell the frame added to
    the mean of its 26 neighbours at its level and of its bucket and itself, by place, samples and an edge test of their
    noise, into filtered faces the lookups read; a cell 25 words, a slot 24 with its cell's place, level and bucket):
    the fold and the filter 0.31 ms, 124 MB. With the level by distance alone (a twentieth), Sponza's corridor settled
    against the clipmap: the mean 6.95 to 7.14 of 255, the lower half's blotchiness 1.49 to 1.44, at 121 pixels 3.71 to
    3.43 (`temp/gi-disocclusion/worldlookup.ps1`). Coarser cells (a tenth) were brighter (7.87): a coarse cell's
    footprint takes other surfaces of the same facing (the floor's sunlit band beside its shade), the levels agreeing
    on their samples but not on what a lookup needs of them.
  - **The go or no-go: no go, as built.** The corridor walk, moving against settled at the walk's three poses
    (`walkp.ps1`, each run the clipmap or the grid throughout): the clipmap 1.16, 1.47 and 1.63 of 255, the grid
    1.48, 1.75 and 1.53, worse at two of three, mostly on the near wall (its strips 0.8 to 1.4 where the clipmap's are
    0.4 to 0.8). Trusting a cell only from 256 samples rather than 16 (so that new cells give way to the clipmap's
    light) changed nothing (1.58, 1.73, 1.49): it isn't the fresh cells taken too soon. For 0.25 ms more of lookups,
    0.4 of filling and 0.3 of folding and filtering, and 124 MB, it is no better settled nor walking.
  What could still change that, untried: the lookups' footprint (a coarse cell takes other surfaces: cells keyed by
  the surface's plane as well as its facing, or a lookup weighting the cells by how far its point is from the samples
  they hold, which a cell would have to keep); a filter as wide as the cache's (three passes, about 7 cells each way:
  two copies of the filtered faces, 36 MB more); and what the walk's difference on the near wall is (the grid's own
  noise, its footprint, or the clipmap's light blended where the cells cover little). The plan stops here, as its step 2
  says, until the user decides whether to go on with those or to leave the clipmap as it is.
- 2026-11-19, **step 2 again, on the user's word** ("continue with those three, and brainstorm for other things to try"):
  - **What the walk's difference was.** Through the grid's own view (`walkp.ps1 -View gi-world`) moving against settled
    was 2.75 to 3.95 of 255: the floor ahead strewn with bright cells, new ones of a few samples, one that met the sun's
    band all of its light. Not the hits' light depending on where the clips are (every hit lit by the last clip: the
    same, 1.64, 1.70, 1.52), nor the frames' noise (settled against itself 16 frames on: the grid 0.38, the clipmap
    0.06). The difference maps (`temp/review/walk-grid-diff-47.png` against `walk-clip-diff-47.png`) showed cell sized
    patches everywhere: each run's own blotches, slow to change, which a moving run and its settled reference hold
    differently. So the grid's spatial noise, not motion.
  - **New cells begun from their parent's light** (`GiWorldSeed`: the cell of the next level up holding it, of its
    bucket, as 64 samples; the samples added coarse first so the parent is there; as the clipmap's cache begins an entry
    new to a clip from the coarser clip's): the grid's view 2.83, 3.56, 3.39; shaded 1.45, 1.71, 1.46.
  - **The filter a-trous, three passes** (taps 1, 2 and 4 cells apart, reaching 7 each way, as the cache's; two copies
    of the filtered faces, the passes taking turns; a cell 34 words): shaded 1.53, 1.63, 1.36, the still image 1.46
    and 3.57 but spotted (bright cells on the vault, blue ones in the floor's band) and 6% darker; 0.77 ms.
  - **The edge test by the cells' variance** (each cell keeps the mean of its samples' squares, of the light along its
    bucket's face, from a sum of squares in the slot; a tap's noise the root of its variance over the samples it stands
    for, as the cache's filter has it): a cell that caught a rare bright sample has the variance to take its neighbours,
    where by its light alone it stood apart as an edge and kept it. The still image 1.33 and 3.37 (the clipmap 1.44 and
    3.43), the walk 1.29, 1.39, 1.11 (the clipmap 1.16, 1.47, 1.63).
  - **The filter along the surface alone** (the 3 by 3 taps across the bucket's axis, not the 27 around: those in front
    and behind are other surfaces facing the same way): the still image 1.32 and 3.38, mean 6.83 (the clipmap 7.14),
    the walk **1.19, 1.31, 1.12**; the fold and filter 0.54 ms rather than 0.77. In Sponza's corridor the grid now
    matches or beats the clipmap settled and walking (the same at 31, better by a tenth at 47, a third at 63).
    `temp/review/world-step2-sheet.png` (the indirect diffuse three times brighter, the clipmap above).
  Costs as they stand, beside the clipmap (Sponza's corridor, 1080p): the frame 4.94 ms against 4.11, of which the
  lookups 0.25, the samples added 0.3, the fold and filter 0.54; 161 MB. What the grid would take of the clipmap's
  (the cache's entries, filter and volumes, steps 3 and 4) is what pays for it.
  - **Beyond Sponza** (the regression set, `ab.ps1` A the clipmap, B the grid, lit, settled 1500 frames): every scene's
    mean within half a level of 255 (the bathroom framed and at the user's two poses, Sponza's columns and corridor,
    Bistro by day and night, CornellBox), every strip within 1.6. By eye the same, but in the bathroom the niches and
    under the window a little darker and a faint band down the back wall beside each shelf (where the wall's cells meet
    the shelf sides' of another facing), and at night Bistro's lamp's pool on the wall smaller. MovingInterior's to and
    fro walk (`pops.ps1`): the steps 96 thousand a million pixels over the walk against the clipmap's 109, the worst
    frame 5.6 against 4.9; but the frames change more from one to the next (9048 against 6250), the grid's noise from
    frame to frame (settled against itself 16 frames on, `noisefloor.ps1`: 0.16 of 255, the clipmap's 0.06).
  - **The corner's bright line came back with the grid**: the entries of MovingInterior's +x wall's hidden part (inside
    the +z wall), which the cache marks inside, gave the grid their rays' samples of the sunlit outside. A sample now goes
    to the grid only from an entry the cache holds valid (no more than a quarter of its rays inside something) once it
    has traced 16 rays (`toWorld` in `GiCacheUpdate`): the line gone, the corridor the same (still 1.33 and 3.36, the
    walk 1.17, 1.37, 1.13).
  - Tried for the cost, not kept: the lookups blended with the next level only in the last quarter before it (Main
    Visibility 0.63 to 0.55 ms, but the walk 1.25, 1.43, 1.21: the levels don't agree well enough for a short hand-over);
    the filter's variance read from the face's two words rather than all nine (no change, 0.54 ms: its cost is the
    hash searches, 3 passes of 9 taps a touched cell). Frame times of single runs vary by a few tenths of a
    millisecond here: the performance work wants interleaved runs.

## Step 1's design, before it is built

- **Many rays add to a cell at once** (every coarse level's cell takes samples from many voxels' rays in a frame), so
  a cell's mean can't be updated in place as an entry's is by the one thread that owns it. Each frame's samples go into
  fixed point sums by 32 bit atomic adds (every backend has them; NVIDIA's SHaRC sums its radiance so), rounded at
  random from the value's bits before the add (a sum rounded toward zero, or to the nearest by a float compare the
  compiler folds, is biased: the half rounding of the cache's faces climbed without end, see the notes), then a pass
  over the cells touched that frame folds each sum into the running mean (its window, as the cache's: up to
  `SPEEDO_GI_CACHE_SAMPLES` samples).
- **The sums aren't kept per cell** (19 words more a cell, 2^21 cells: 150 MB more): a cell touched in a frame takes a
  slot of the frame's sums (its index in the cell, tagged with the frame, set by compare and exchange), from a buffer
  sized by the most cells the frame's rays can touch (the cache's rays a frame times the levels): a capacity, failing
  hard where it runs out, as the others do.
- **Per ray**: a cell at each level found or inserted (a hash, a probe of up to 8 slots), and 9 atomic adds and a count
  in each (the three faces a direction lights, in rgb): 50 atomics a ray at 5 levels, a few million a frame. Its cost
  is step 1's gate, beside the table's memory.
