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
