# Performance plan: frame rate and convergence in heavier scenes

The client's look is close to its targets; its frame rate and the time its lighting takes to settle are not, in the
heavier scenes (Bistro, San Miguel). The client is to grow into a cross-platform game platform rendering richer
environments and worlds, so what is chosen here is chosen for that: general methods that scale with content and run
on every platform, with a platform's exceptions named for what it lacks (see CLAUDE.md, "Portable by default, measured
on one").

## The problem

The two goals are one problem twice. The indirect lighting settles over frames, not seconds: the probes blend 3% of a
frame's rays in (`SPEEDO_GI_HYSTERESIS`, 0.97, about 100 frames to settle), towards the mean of a 32 frame cycle of ray
sets, the bounce waits 32 frames after a reset, the surface cache averages up to 1024 samples, the emitters 32 frames
and the near field 16. At Bistro's frame on the M4 Pro (about 75 ms at 2560x1440, roughly 13 fps), 100 frames are 7 to 8
seconds. A faster frame shortens convergence, and convergence can be made faster on its own.

What was recorded before this plan (CLAUDE.md, measured at different times; Bistro's street, KosmicKrisp, 2560x1440,
the `Frame` scope): traced shadows about 5 ms, the emitters 4 to 5, the indirect lighting about 11 and its surface cache
about 3 more, the shadow pass about 4, Main about 6 (its indirect lighting lookups 2.4), and a sun that moves every
frame about 10 more. The rest is unaccounted for: KosmicKrisp times neither compute passes nor Main Late. Nothing was
recorded for San Miguel.

## The benchmark poses

The user's, both at 2560x1440 with the indirect lighting on:

- Bistro in the evening: `SPEEDO_CAMERA=-800.3088,154.0938,132.3450,-4.860,71.352
  SPEEDO_AUTOLOAD_MODEL=resources/test-assets/mcguire/bistro/Exterior/exterior.obj SPEEDO_TIME_OF_DAY=20.000`
- San Miguel: `SPEEDO_CAMERA=8.5157,1.6398,2.3221,-6.609,-251.093
  SPEEDO_AUTOLOAD_MODEL=.speedo/archives/San_Miguel-4b7e8c9d0012b87a/san-miguel.obj SPEEDO_TIME_OF_DAY=10.500`
- Sponza (Khronos' glTF): `SPEEDO_CAMERA=-8.6117,0.7801,3.0412,0.609,-269.373
  SPEEDO_AUTOLOAD_MODEL=resources/test-assets/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf SPEEDO_TIME_OF_DAY=10.500`

## The steps, in order

### 1. A measurement baseline

Before changing anything, per scene and machine (the M4 Pro through KosmicKrisp, the RTX 4070 Ti SUPER on Windows):

- **What each feature costs**: the draw thread's frame time (the gpu's, where the draw thread waits on its fences; the
  `Frame` scope misses most compute work on KosmicKrisp, see the results) with everything on, then with each feature off
  in turn (`SPEEDO_GI=0`, `SPEEDO_TRACED_SHADOWS=0`, `SPEEDO_EMISSIVE_LIGHTS=0`, `SPEEDO_GI_CACHE=0`, `SPEEDO_GI_NEAR=0`,
  `SPEEDO_SHADOWS=0`), all of them off (the raster floor), and with the sun moving (`SPEEDO_TIME_SPEED`). The default
  run first and last, to bound the gpu clock's drift.
- **What each pass costs**: the frame time freed with one pass left out (`SPEEDO_SKIP_PASSES`), since KosmicKrisp's
  timestamps miss most passes; Nsight on Windows times each dispatch too.
- **How long the lighting takes to settle**: from a cold start at the pose, at a fixed exposure (auto exposure settles
  by time and would be measured with it), every 10th frame captured (`SPEEDO_CAMERA_PATH` of the pose repeated,
  `SPEEDO_CAPTURE_PATH`, `SPEEDO_CAPTURE_PATH_STRIDE`), each against the settled image (the mean of the last captures):
  the frames until the mean |log2| luminance difference (blurred over 24 pixels: the lighting's error, not its grain)
  stays under 0.1, 0.05 and twice the image's noise (the last captures' spread: a difference that stays near it is
  noise, not unsettled light), and in seconds at the frame time measured above. Still to add: the same after a change, a
  jump of the camera and a change of the time of day.

Repeatable on both machines, so that every later step reports against it.

### 2. Converge faster, whatever the frame rate

- **Time-based accumulation**: the blend rates and history lengths (the probes' hysteresis, the surface cache's sample
  cap, the near field's and the emitters' histories) expressed as times, as auto exposure is, with a floor in samples, so
  that the settling time doesn't grow as the frame rate drops. The cost is more noise at a low frame rate.
- **More rays after a reset or in a new area**: the ray budgets raised for the first frames after a reset, a scroll or a
  scene change, and more rays a voxel while a cache entry is young.
- **A shorter chain of delays** where the light changes: the 32 frame cycle a probe's target is the mean of, the 32
  frames the bounce waits after a reset, and the hysteresis, which add up.
- **A warm start**: the settled state (the probes, the surface cache, the voxels' kept light) saved per scene and time of
  day and loaded with the scene, so that a level starts settled: a baked starting point, updated live. For a game this
  is the expected model.
- **The time of day's relighting**: the static voxels lit again only once the sun has moved past a threshold (as the sky
  is drawn again), spread over frames, rather than every frame (about 10 ms of Bistro's).

Measured by step 1's settling times, in seconds, and the still frame's noise (its frame to frame change).

### 3. Fewer pixels: temporal upscaling

Rendering at about 0.67 of the output's size and upscaling cuts every pass that runs per pixel (the pre-pass, Main, the
traced shadows, the near field, the emitters, ComputeMain) to about 45% of its pixels: the biggest single lever on the
frame rate, on every platform. FSR first (open and portable), DLSS and MetalFX where present later. It needs motion
vectors, moving geometry's included (skinned, morphed, following animated nodes), and a jittered projection; and a
shared temporal pass could stand in for the separate temporal filters the traced shadows, the near field and the
emitters each run now. Measured by step 1's costs, and by A/B captures against the native resolution at the poses.

### 4. Cheaper rays: hardware ray queries behind the software BVH

Rays are now in nearly every system: the traced shadows, the emitters' shadow rays, the near field, the probes' rays,
the probes' visibility, the voxels' shadow rays and the surface cache. `VK_KHR_ray_query` in the compute kernels where
the device has it (NVIDIA, AMD and Intel desktop gpus; the M4 Pro has hardware ray tracing in Metal, whether KosmicKrisp
exposes it is to be checked), and tinybvh's software traversal where it doesn't, the exception named for what the
device lacks. It needs acceleration structures per model instanced into the scene, which also cut the memory a
flattened world takes (Bistro's BVH is 193 MB) and are the first part of a scene of instances (step 5). Meanwhile, on the
software path: better static builds (spatial splits, tinybvh's high quality builder), simplified geometry for the
indirect lighting's rays, ray budgets driven by the frame time, fewer rays for probes that have settled, and the
coarser clips updated every few frames.

Also in this step, what changes slowly done less often: the shadow maps of static casters cached, the sun's redrawn past
an angle (virtual shadow maps the longer term version).

### 5. Rich worlds

As content grows: a scene of model instances rather than one flattened mesh; streaming, with level of detail for meshes
(meshoptimizer's cluster LOD) and textures; async compute on the desktop gpus' compute queues; recording on several
threads (a command pool per thread); quality presets and a controller of the resolution and the ray budgets aimed at a
frame time, which make the traced shadows' and the emitters' density choices of a tier. Longer term, a native Metal
backend (rhi is templated on the graphics api for it): MetalFX, Metal's ray tracing, real gpu timings, and no
translation layer.

## Baseline results

`scripts/perfbaseline.ps1` runs all of it (`-Features`, `-Passes`, `-Convergence`) and writes the tables; the
settling analysis is `build/<preset>/convergence` (`src/tools/convergence.cpp`). Pass skips are
`SPEEDO_SKIP_PASSES=<pass>[;...]` (the GI pass's steps by name, see `framegraph::Skipped`), the window's size in pixels
`SPEEDO_WINDOW_SIZE`, and a long camera path `SPEEDO_CAMERA_PATH=@<file>`.

### How to read the numbers

On KosmicKrisp the `Frame` scope misses most compute work: Bistro's frame reads 43.4 ms by it and 67.3 ms by the draw
thread, which a sampling profiler shows waiting on its fences (482 of 500 samples in `Fence::Wait`): the gpu's time. In
San Miguel the two agree within 2 ms; leaving out a GI pass doesn't move Bistro's `Frame` scope at all. So the draw
thread's frame time (fifo, the median of its half seconds) is the measure here, and a feature's or pass's cost is the
frame time it frees. Measurements of compute heavy features by the `Frame` scope in CLAUDE.md are undercounts on
KosmicKrisp: the near field's 0.9 ms is 16 to 19 ms at these poses. A pass left out can change what others do: GI Light
and GI Flags gate everything after them, GI Cache Release the cache's work, ComputeMain's block marks the cache's
active blocks (its own cost, measured with the cache off, is 4.5 ms of Bistro's), the shadow atlas the
classification's rays, the depth pyramid the late draws. The defaults agree within 0.3 ms in Bistro and 3 ms in San
Miguel (its middle default ran slow), so San Miguel's figures carry about 1.5 ms.

### The M4 Pro (KosmicKrisp), 2560x1440, 2026-10-27

Bistro's evening pose: **67.5 ms a frame (15 fps)**; the sun is at the horizon at 20:00, so the shadows cost nothing and
the light is the emitters' and the sky's. San Miguel: **86 ms (12 fps)**, the sun up. Sponza, looking into its
interior, lit mostly by the bounce: **37 ms (27 fps)**.

What each feature frees (each off in turn):

| feature | Bistro (ms) | San Miguel (ms) | Sponza (ms) |
|---|---|---|---|
| indirect lighting (all of it) | 43.5 | 32.0 | 25.7 |
| its near field | 18.9 | 16.4 | 9.9 |
| its surface cache | 11.1 | 7.2 | 5.6 |
| the emitters | 10.8 | 6.7 | 0 |
| the traced shadows (the atlas's soft filter in Main instead) | 0 | 2.0 | 0.5 |
| the shadows (atlas, traced, and Main's filtering) | 0 | 26.7 | 4.1 |
| all of them (the raster floor: 12.7, 21.0 and 8.3 ms) | 54.7 | 65.0 | 28.7 |

What each pass frees (left out alone):

| pass | Bistro (ms) | San Miguel (ms) | Sponza (ms) |
|---|---|---|---|
| GI Near (the near field's rays, every other pixel each way) | 10.4 | 9.6 | 5.7 |
| GI Near X and Y (its filter, every pixel) | 4.4 | 7.7 | 7.6 |
| GI Near Temporal | 1.0 | 0.8 | 1.7 |
| GI Resolve (with the cache's ray per voxel and the probes' visibility rays) | 12.6 | 8.2 | 12.2 |
| GI Trace (the probes' rays: 3756 and 1664 at 128 rays, 4069 at 120) | 7.3 | 2.4 | 2.4 |
| GI Cache Filter | 3.3 | 2.7 | 2.6 |
| GI Probe Update | 1.5 | 0.7 | 1.3 |
| Emitter Sample (8 candidates a traced pixel) | 8.6 | 3.9 | 0.2 |
| Emitter Shade (the shadow rays) | 5.3 | 1.2 | 0.1 |
| Main | 9.4 | 15.8 | 4.2 |
| ComputeMain (4.5 its own in Bistro) | 14.5 | 10.7 | 7.4 |
| Shadows (the atlas: San Miguel's two sided casters, 4 cascades) | 0 | 9.9 | -7.4 (every pixel traced then) |
| Shadow Classify (with the rays it sends) / Trace / Denoise X and Y | 0 | 7.5 / 3.0 / 3.3 | 2.3 / 0.5 / 1.0 |

How long the lighting takes to settle from a cold start (the difference from the settled image at a 24 pixel blur,
`convergence`'s default; the noise is the last captures' spread):

| | within 10% | within 5% | within twice the noise | noise (24 px / 4 px) |
|---|---|---|---|---|
| Bistro | 70 frames (4.7 s) | 140 (9.5 s) | 800 (54 s) | 0.014 / 0.033 |
| San Miguel | 130 (11 s) | 350 (30 s) | 1090 (94 s) | 0.005 / 0.014 |
| San Miguel, surface cache off | 110 (9.5 s) | 150 (13 s) | 360 (31 s) | 0.004 / 0.012 |
| San Miguel, bounce off | 70 (6 s) | 320 (28 s) | 1090 (94 s) | 0.006 / 0.014 |
| Sponza | 410 (15 s) | 760 (28 s) | 1080 (40 s) | 0.005 / 0.007 |

What changes late is the light in what only the indirect lighting lights (San Miguel's arcade, the shade under its
trees, its far wall), brightening: the surface cache's running mean (up to 1024 samples, a ray per voxel a frame, a
block's page a frame late and its rays every 8th frame until a lookup reads it) is the slow tail; without it the probes
settle within their noise in 360 frames. Bistro at night is noisy (the emitters', 2.5 times San Miguel's), and drifts
slowly within it.

### What it says for the next steps

- **Convergence (step 2)**: the surface cache first: its sample count by time rather than frames, more rays a voxel
  while an entry is young, its pages taken without the frame's delay, and a warm start. Then the probes' chain (about
  150 frames to 5%: the hysteresis, the 32 frame cycle and the bounce's 32 frame wait).
- **The near field is the largest single item** (16 to 19 ms): its rays (10) reach 8 voxels of the clips that light a
  point, metres at a distance in the coarse clips, and its filter (4 to 8) runs at every pixel. Shorter rays far away,
  the filter at the traced pixels' resolution, and the upscaling (step 3) are the levers.
- **Rays** are most of the rest: the resolve's (12.6 ms in Bistro), the probes' (7.3), the emitters' (5.3), the
  traced shadows' (6): step 4.
- **Per pixel shading**: Main (9 to 16 ms), ComputeMain (4.5), the near field's filter: step 3.
- **San Miguel's shadows** (26.7 ms in all; the atlas pass 9.9, its 4 cascades of two sided casters): cached static
  casters, and the traced shadows' passes (about 11 ms, against the atlas's soft filter in Main costing nearly as much).

The RTX 4070 Ti SUPER's numbers are still to be measured (`pwsh scripts/perfbaseline.ps1 -Preset
x64-windows-clang-profile`).

## Progress

### Step 2, the first evening (2026-10-27)

What was tried on the cache's tail, by the settling time from a cold start (`scripts/perfbaseline.ps1 -Convergence`,
and runs of 2400 frames, since at 1200 the "settled" captures were still moving):

- **Kept: the probes' hysteresis by time** (`SPEEDO_GI_HYSTERESIS` is the blend at 60 frames a second; at 27 frames a
  second 0.935, at 12 0.86): with the cache off, Sponza within 5% after 170 frames rather than 220. Free on a still scene
  (a probe blends towards its last whole cycle's mean, the same directions each cycle).
- **Kept: the cache's window at most half the frames since it started over**: Sponza within 10% after 460 frames rather
  than 840. The tail is unchanged: Sponza still changes 2% 2000 frames in, either way.
- **Kept: 4 rays a voxel for the cache's first 256 frames** (then 1): Sponza within 10% after 280 frames rather than
  460, for 4.8 ms a frame over those frames (4 rays throughout: 240 frames, but 4.8 ms always and noisier); its 5% point
  and noise as before.
- **Kept: the cache's inside rays over the samples' window** (a bug: clamped apart, a voxel with a tenth of its rays
  inside went dark after 3400 frames). Few of Sponza's voxels have any, so it changed nothing measurable there.
- **Tried and dropped**: a lower cap (128 or 216 samples: five times the noise, no sooner settled), the samples weighed
  by their place (2 / (n + 1): noisier, the same curve), the cache following its probes' change at once (overshot from
  a cold start, since the probes start from nothing; gated after 64 frames, no change), the probes' light as a prior
  ramped to full at 32 samples (no change), its pages never released and every block active (no change), 8 ray phases
  and a hysteresis of 0.9 (no change: the probes aren't what is slow with the cache on).

What it leaves: the cache averages, at a ray a voxel a frame, a light that is itself still settling (the bounce builds up
through it), and a mean whose window grows with time forgets that only as 1 over the frames; a fixed short window
forgets it geometrically but is five times as noisy. With the cache off, Sponza is within its noise after 600 frames,
with it not after 2400. The next things to try: more rays a voxel while the light settles (the plan's "more rays after a
reset"), measuring the cache's light against its probes' rather than alone (so that what it averages doesn't move as the
light builds up: tried naively above, it needs the probes' warm-up handled), and a warm start.

The three kept together, by the settling from a cold start (frames, and seconds at the baseline's frame times):

| | within 10% | within 5% | within twice the noise | noise (24 px) |
|---|---|---|---|---|
| Bistro, before | 70 (4.7 s) | 140 (9.5 s) | 800 (54 s) | 0.014 |
| Bistro, after | 70 (4.7 s) | 100 (6.8 s) | 400 (27 s) | 0.014 |
| San Miguel, before | 130 (11 s) | 350 (30 s) | 1090 (94 s) | 0.005 |
| San Miguel, after | 100 (8.6 s) | 420 (36 s) | 1090 (94 s) | 0.007 |
| Sponza (2400 frames), before | 840 (31 s) | 1440 (53 s) | 2140 (79 s) | 0.007 |
| Sponza (2400 frames), after | 460 (17 s) | 1460 (54 s) | 2160 (80 s) | 0.007 |
| Sponza (2400 frames), after, with the warm-up rays | 280 (10 s) | 1480 (55 s) | 2200 (81 s) | 0.007 |
| Bistro, with the filter held (second round) | 50 (3.4 s) | 80 (5.4 s) | 170 (11.5 s) | 0.014 |
| San Miguel, with the filter held | 80 (6.9 s) | 310 (26.7 s) | 1080 (93 s) | 0.007 |
| Sponza (2400 frames), with the filter held | 240 (8.9 s) | 960 (35.5 s) | 2220 (82 s) | 0.006 |
| Bistro, with the cache's rays split (third round) | 50 (3.4 s) | 80 (5.4 s) | 280 (19 s) | 0.014 |
| San Miguel, with the cache's rays split | 90 (7.7 s) | 300 (25.8 s) | 1060 (91 s) | 0.007 |
| Sponza (2400 frames), with the cache's rays split | 160 (5.9 s) | 620 (23.0 s) | 2180 (81 s) | 0.005 |

San Miguel's 5% comes later: the cache's shorter early window is noisier while it settles. The step isn't done: the
cache's tail is what is left of it.

`SPEEDO_GI_TIME_BASED=0` restores the per frame hysteresis and the cache's whole window, for comparisons.

### Step 2, the second round (2026-10-27 night)

What the tail was, found by dumping a point's probes and cache entry at frames 300, 1000 and 2400
(`SPEEDO_GI_DUMP=pixel:...`): the probes were identical to five digits from frame 300 on, the cache entry's light only
noisy; what drifted for 2000 frames was the cache's filter. Its edges stop at the noise of the voxels' means, a variance
over the voxel's samples, which shrank as they grew to 1024, so the filter smoothed less and less and the light it had
spread into darker voxels withdrew: the image darkened as the samples grew, whatever their window. Held at the noise of
a mean of 64 samples (`kGiCacheFilterCount`; 32 the same), Sponza comes within 10% of settled after 240 frames (280
before) and within 5% after 960 (1480), the settled image within 0.03 of before.

- **Tried and dropped**: restarting the cache's entries at 256, 512 and 1024 frames (32 samples kept: the image jumped
  away from settled at each, the loosened filter brightening it, which is what pointed at the filter); the bounce taking
  the probes' light rather than the cache-blended volumes, to take the cache out of the loop the bounce makes (no faster,
  and 9% brighter); a cap of 256 samples with the filter held (three times the noise: the cache needs its long window).
- **What is left**: the window's growth, half the frames since the cache started over, to 1024 at frame 2048, forgets
  the first frames' samples as 1 over the frames (Sponza 2% off at frame 2000). Shortening it costs noise (a voxel's
  light is mostly the few rays that find a bright opening), so what remains is lower variance sampling (rays guided
  toward where the light comes from) or a warm start (the settled cache saved with the scene).
- **A watchdog fix found on the way**: after a shader change, the draw thread's first frames create the GI's compute
  pipelines cold, over 20 s in all on KosmicKrisp, and the crash watch reported a hang at startup (three of the quick
  tier's first runs, and a measurement): a pipeline the draw thread creates now counts as its heartbeat.

### The near field at the traced pixels (2026-10-27)

Its filter (GI Near X and Y) and its frames (GI Near Temporal) ran at every pixel though the rays are traced at one
pixel in four (5.4, 8.5 and 9.3 ms of Bistro's, San Miguel's and Sponza's frames); they run at the traced pixels now,
and the main pass upsamples by the traced pixels' surfaces, as the emitters' light (`GiNearUpsample`). Frame times:
Bistro 67.5 to 65.4 ms, San Miguel 86.0 to 84.2, Sponza 37.1 to 33.9. The images differ from before by about twice the
frames' noise (Sponza at frame 600: 0.028 mean |log2| at 4 pixels, its frames 0.012 apart; CornellBox's spheres
0.016), a contact a few pixels wide (Sponza's curtains on the floor) a little lighter. `SPEEDO_GI_NEAR_STRIDE=4` traces
a sixteenth of the pixels instead of a quarter: Bistro 65.4 to 54.3 ms, San Miguel 84.2 to 75.2, Sponza 33.9 to 29.7,
its frames barely noisier (Sponza's 0.014 apart against 0.012), but coarser at contacts (Sponza at frame 600 0.042
from stride 2, three times the frames' noise: the curtains' contact on the floor lighter over a wider band, a few darker
spots along the sunlit edge). Left at 2 for the user to judge.

### Step 2, the third round (2026-10-27 night)

- **Kept: the surface cache's rays split by the voxel's open side**: three quarters over the hemisphere it faces and a
  quarter over the other, each sample weighed by its side's density (`kGiCacheForwardShare`, `GiCacheEstimate`). Over
  the sphere evenly half the rays added little to the faces the lookups read; over the open side's hemisphere alone
  Sponza settled 6% darker (0.19 mean |log2| from before: the lookups read the other side's faces too, at edges and
  corners, and the air voxels' open sides are only their occupancy's gradient). Split, the settled images are as before
  (Sponza 0.025, Bistro 0.012, San Miguel 0.033, about their noise), Sponza comes within 10% of settled after 160 frames
  rather than 240 and within 5% after 620 rather than 960, its noise 0.0052 rather than 0.006; Bistro and San Miguel as
  before (the table above). No cost (Bistro 80.7 ms against 81.1, interleaved twice).
- **Kept: ComputeMain a thread per pixel**: it ran a thread per 16x16 pixels, 14400 threads at 2560x1440, too few to
  hide its reads. Its own cost in Bistro (with the cache off, whose block marks it also does) 2.2 ms rather than 4.5; the
  images are identical.
- **Found on the way: the warm-up rays' cost.** The cache's 4 rays a voxel for its first 256 frames (the second round)
  cost Bistro 15 ms a frame while they last (80.3 ms against 65.4), San Miguel 6 (94.4 against 88.1) and Sponza 4.5 (35.9
  against 31.3), and a run of 400 frames measured mostly them: the client now prints the frames after the warm-up apart
  (`Renderer::IsLightingWarmingUp`, "frame time" and "frame time warming up"), `perfbaseline.ps1` runs 600 frames and
  gives `convergence` both (`--warm-ms`, `--warm-frames`), so that its seconds count the warm-up at its own frame time.

The settled frame times now (2560x1440, fifo, after the warm-up, frames 256 to 700): Bistro 65.4 ms, San Miguel 88.1
(87.4 to 88.9 over four runs; 84.2 at the near field's round, over frames 100 to 400 of another session: the gpu's state
drifts between sessions, so compare within one), Sponza 31.3 (33.9).

What is left of step 2: a warm start (the settled cache and probes saved with the scene), and the time of day's
relighting spread over frames. Step 3, the upscaling, cuts every per pixel pass, which is now most of the frame.

### Step 3, the first version (2026-10-28 night)

Temporal upscaling of our own (CLAUDE.md, "Temporal upscaling"): View > Upscaling, `SPEEDO_UPSCALING=off|native|quality|
balanced|performance` (1, 1, 0.77, 0.67 and 0.5 of the output's size each way), off by default until judged by eye. The
frame rendered at the render size with jittered projections, composited there (ComputeMain), resolved into a history at
the output's size (reprojected by depth, clipped to the neighbourhood, blended by sample weight), tonemapped and
sharpened (RCAS) into the swapchain; the textures' mips biased by the render scale and half a level more.

| | off | native | balanced | performance |
|---|---|---|---|---|
| Bistro (ms) | 65.1 | 64.6 | 47.7 (-27%) | 40.9 (-37%) |
| San Miguel | 86.6 | 83.1 | 65.0 (-25%) | 57.3 (-34%) |
| Sponza | 31.5 | 33.3 | 26.5 (-16%) | 23.8 (-24%) |

What scales with the pixels went to 45% and 25% of its cost; the rest (the probes, the voxels, the surface cache, the
shadow atlas, the culling) didn't. Still images at balanced are a little softer than off, native as sharp (Sponza's
stone and window grate at 2x zoom); turning at 30 degrees a second shows no ghosting. Then motion vectors for what moves (skinned, morphed, following animated nodes: the pre-pass writes each pixel's motion
since the last frame, from where the vertex stage places those draws in the last frame slot's buffers), which the
resolve takes where the nearest pixel moves: CesiumMan walking and the Fox show no trails. What it lacks: FSR, DLSS and
MetalFX behind the same interface, and the temporal filters of the near field and the emitters taking the upscaler's
history instead of their own.
