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
the light is the emitters' and the sky's. San Miguel: **86 ms (12 fps)**, the sun up.

What each feature frees (each off in turn):

| feature | Bistro (ms) | San Miguel (ms) |
|---|---|---|
| indirect lighting (all of it) | 43.5 | 32.0 |
| its near field | 18.9 | 16.4 |
| its surface cache | 11.1 | 7.2 |
| the emitters | 10.8 | 6.7 |
| the traced shadows (the atlas's soft filter in Main instead) | 0 | 2.0 |
| the shadows (atlas, traced, and Main's filtering) | 0 | 26.7 |
| all of them (the raster floor: 12.7 and 21.0 ms) | 54.7 | 65.0 |

What each pass frees (left out alone):

| pass | Bistro (ms) | San Miguel (ms) |
|---|---|---|
| GI Near (the near field's rays, every other pixel each way) | 10.4 | 9.6 |
| GI Near X and Y (its filter, every pixel) | 4.4 | 7.7 |
| GI Near Temporal | 1.0 | 0.8 |
| GI Resolve (with the cache's ray per voxel and the probes' visibility rays) | 12.6 | 8.2 |
| GI Trace (the probes' rays: 3756 and 1664 probes at 128) | 7.3 | 2.4 |
| GI Cache Filter | 3.3 | 2.7 |
| GI Probe Update | 1.5 | 0.7 |
| Emitter Sample (8 candidates a traced pixel) | 8.6 | 3.9 |
| Emitter Shade (the shadow rays) | 5.3 | 1.2 |
| Main | 9.4 | 15.8 |
| ComputeMain (4.5 its own in Bistro) | 14.5 | 10.7 |
| Shadows (the atlas: San Miguel's two sided casters, 4 cascades) | 0 | 9.9 |
| Shadow Classify (with the rays it sends) / Trace / Denoise X and Y | 0 | 7.5 / 3.0 / 3.3 |

How long the lighting takes to settle from a cold start (the difference from the settled image at a 24 pixel blur,
`convergence`'s default; the noise is the last captures' spread):

| | within 10% | within 5% | within twice the noise | noise (24 px / 4 px) |
|---|---|---|---|---|
| Bistro | 70 frames (4.7 s) | 140 (9.5 s) | 800 (54 s) | 0.014 / 0.033 |
| San Miguel | 130 (11 s) | 350 (30 s) | 1090 (94 s) | 0.005 / 0.014 |
| San Miguel, surface cache off | 110 (9.5 s) | 150 (13 s) | 360 (31 s) | 0.004 / 0.012 |
| San Miguel, bounce off | 70 (6 s) | 320 (28 s) | 1090 (94 s) | 0.006 / 0.014 |

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
