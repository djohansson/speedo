# World cache plan: the indirect lighting's light anchored in the world

The surface cache and the probes keep their light per clip of the voxel clipmap, and the clips disagree about the same place (docs/notes/indirect-lighting.md, "Why the clip levels disagree"). Walking, the disagreement shows as patches that travel with the camera, and a clip's scroll or its weight's hand-over shows one clip's light for another's. This plan keeps the surfaces' light instead in a hash grid of cells anchored in the world at several resolutions, each sample added to every level, so that the levels are means of the same samples and agree by construction. It is built side by side with the clipmap's light, selectable, until shown better in visual quality, performance and memory. Status: steps 0 to 2 and 4 are done and step 3 was dropped. The grid works and is selectable (View > Rendering Features > Global Illumination... > Light Store: Clipmap or World Grid) but is off by default (`SPEEDO_GI_WORLD=1` turns it on): it costs more than the clipmap a frame, and buys steadier light while walking. The clipmap keeps the air's light, what moves, the fog and what traced rays meet. The clip levels' disagreement it was begun for is met in the clipmap instead (docs/backlog.md). A neural alternative was measured and set aside (docs/notes/indirect-lighting.md, "A neural radiance cache").

## The structure

A hash table of cells of fixed capacity (a policy, not a capacity to fail on: the stalest cells are taken back), open addressing with a bounded probe, about a million cells by default (`SPEEDO_GI_WORLD_CELLS`), a cell a few dozen words (its light along the axes as an ambient cube, its sample count, its mean of squares, where its samples lie, its filtered light).

- **A cell** is a level L (its side doubles per level, `SPEEDO_GI_WORLD_LEVELS` of them), the world position divided by that side and floored, and a normal bucket (the facing of the surface, so that the two sides of a thin wall are different cells). Its key is a second hash independent of the one its probe starts at (`GiWorldHash`, `GiWorldKey`), since a single hash let two places share a cell.
- **Every sample is added to its cell at every level from its clip's up**: no finer than the voxel its ray started in. A sample comes only from a surface cache entry the cache holds valid, and a ray starting on a surface facing another way than its voxel is not added. Samples are summed per frame into fixed point sums by 32 bit atomic adds, rounded at random to avoid bias, held in per-frame slots (a quarter of the cells), and a fold pass (GI World Fold) folds them into each cell's running mean, over a window that is a time rather than a count of samples.
- **A frame's adds are a budget**: a cell takes a clip's samples every n-th of the clip's updates (`GiData::worldStride`, doubled where cells take most of the slots, halved where few), and a level whose cells would take too many samples of a voxel takes that share at random (`kGiWorldCellSampleCount`). Samples that still find no slot are left out and counted in the timings.
- **New cells** begin from their parent's light at the next level up (`GiWorldSeed`).
- **The filter** (`ComputeGiWorldFilter`, in the fold's pass) is an a-trous filter of three passes along the surface only, with the edge test by the cells' variance, reading cells by how recently they took samples (`kGiWorldStaleNear`).
- **A lookup** (`GiWorldFaces`, `GiWorldBlend` in `GiSampleSurface`) takes the level of the clipmap's voxel size at the point (`GiWorldClipLevel`), blended with the next by the fraction, trilinear across the surface over cells of its bucket, each by its samples over 16. Cells are taken by how near their samples lie to the point, and each by how much of the way to its samples is clear of the voxels (`GiWorldReach`), so light does not come through walls. Where the cells hold too little, or on what moves, the clipmap's light is read.
- **Only the main pass reads the grid**: the traced rays' hits (reflections, refractions) take the clipmap's light.
- **Side by side**: the clipmap stays built and running whatever the switch says. The grid's storage and passes exist only while it is chosen, and switching clears the other's unused structures. Debug view `gi-world`, the exit report's `indirect lighting, world cache:` line, and the memory report (`SPEEDO_GPU_TIMINGS=1`).
- **Source**: `giworld.slang`, `GiCacheUpdate` (the samples' source), the passes GI World Clear and GI World Fold in the renderer.

## What it has to replace

The surface cache (its entries, filter and volumes) for the surfaces, and the probes' four roles, measured in docs/notes/indirect-lighting.md, "The probes, resolved where they show":

1. **The fog** reads the probes (`AerialPerspectiveAmbient`).
2. **What moves**: a moving thing's voxels are new every frame, so its light is mostly the probes'.
3. **The air's light**: the near field's rays that meet nothing (`GiNearTrace`), what lets light through from behind, and lookups beside a surface voxel of few samples.
4. **Thin things**: lookups beside window frames, shutters and recesses shallower than a voxel take the air voxels' light.

The grid replaced none of these (step 4): air cells did not pass their gate and the volumes are what planned volumetric effects (clouds, fog banks) would sample. The grid is the surfaces' light, the clipmap's volumes, probes and cache filter the air's.

## How it is judged (every step)

- **Visual quality**: a regression set of scenes (the bathroom, Sponza's columns and corridor, Bistro by day and night, CornellBox), a corridor walk (moving against settled), a to-and-fro walk of a moving interior, noise between seeds, thin shelves, and the user's eye on each.
- **Performance**: the `Frame` scope and the GI passes' gpu timings at the benchmark poses (docs/performance-plan.md), warm pipelines, runs interleaved and serial.
- **Memory**: each structure's buffers and images by name, from rhi's allocations, the clipmap's and the grid's side by side.

## The steps

### 0. The baseline

Done. The clipmap's memory, GI pass times and quality measures at two resolutions, and the memory report. The clipmap's memory is dominated by its dense voxels and volumes, cubic in the resolution; the paged surface cache is a tenth of it, so the grid's memory gain would be in what it takes from the volumes, voxels and probes, not in the cache it replaces first.

### 1. The grid, filled and shown, nothing shaded from it

Done. The table, insertion, eviction, the cache's rays adding samples at every level from their clip's up, the `gi-world` debug view, and the exit report. Adding the samples costs a fraction of a millisecond beside the clipmap.

### 2. The surfaces shaded from the grid (the go or no-go)

Done after several rounds of work. As first built the grid was no go: mottled and no better walking. It became go with the cells filtered (a-trous, by variance, along the surface), new cells seeded from their parent, keys that do not collide, where-the-samples-lie and reach tests in the lookups, stale cells weighted down, the window as a time, lookups at the clipmap's voxel size, the sample budget, and a facing test on the rays added. In Sponza's corridor it matches or beats the clipmap settled and walking, and elsewhere in the regression set it is close to the clipmap with a few small differences (darker niches, a faint band beside shelves, a smaller lamp pool at night). It settles more slowly than the clipmap and is blotchier in its first seconds. Possible further work: a filter as wide as the cache's, with the cache's per-tap test of what lies between (`GiCacheFilterBlocked`), which needs a third set of faces or two passes.

### 3. The cache's rays from the grid's cells

Dropped. The cache's light reaches the image through the voxels' bounce, the volumes (the cache filter's last pass) and the entries themselves (also the rays' sources). Taking the bounce from the grid (3a) and leaving the cache filter's blend out (3b) both made the still image and the walk worse for no gain until the cache's own light goes, and cutting the entries to what a ray's source needs (3c) waited on 3b. The cache filter puts the cache's light into the volumes the air is read from, so it stays.

### 4. The air cells, one role at a time

Closed with the air left to the clipmap. Air cells for the near field's misses cost frame time for no visible gain, and would need a visibility and lookups of their own to stand in for the volumes. The thin things passed with the surfaces' lookups on the grid and the volumes' where the grid has no cell.

### 5. The user's decision

Decided: the grid is off by default and selectable, and on it serves the main pass alone. What would still change that is the lookups' and fill's cost coming down. What stays either way: the voxels where rays walk them (`SPEEDO_GI_TRACE=voxels`, a model without a BVH), which find no surface to anchor at.
