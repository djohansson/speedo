# Visual testing plan: what the client draws, checked

Since 2026-11-11 CI runs the client itself, headless on lavapipe (mesa's Vulkan driver on the cpu: the mesa port's, on
Linux and Windows, and on macOS since 2026-11-13; Ubuntu's on Linux at first, see "CI" in docs/notes/build.md). What those runs check is that the client runs, not what it
draws: a model that comes out black or garbled passes as long as nothing crashes or reports an error. This plan adds checks
of the output, in an order where each step is useful on its own, and the non-visual checks a cpu driver makes cheap.

The user's ask, 2026-11-11: write the plan down and start once the lavapipe CI work is on master.

## What is checked today

- **Client runs** (`assettest.ps1 -Client`, the quick tier locally, BumpDomes on CI until 2026-11-12, when the validated
  visual checks took its place there): each model loaded, uploaded and
  drawn until `SPEEDO_AUTOLOAD_EXIT` frames after its install; a run fails on a crash or timeout, a load failure, a failed
  assert, a validation error (`-Validate`) or a sanitizer report. The split screen runs (`splitscreen.txt`) add four views,
  each with its own exposure, shadow cascades and indirect lighting clips. Nothing looks at the frames.
- **assettest**, on the cpu: the importers (each image's decode against a reference decode, mip levels, the materials'
  textures), the BVH ray by ray against brute force triangle tests, the lens prescriptions. The glTF samples'
  `screenshot/` images are imported as images, never compared with.
- **Tools used by hand**: `SPEEDO_CAPTURE` (the frame an autoload exit ends on, as a bmp), poses (View > Camera > Copy
  Pose, `SPEEDO_CAMERA`, `SPEEDO_CAMERA_PATH` with `SPEEDO_CAPTURE_PATH`), the debug views (`SPEEDO_DEBUG_VIEW`, among them
  the references `DEBUG_VIEW_TRACED_SHADOW_REFERENCE` and `DEBUG_VIEW_EMITTERS_REFERENCE`), `src/tools/convergence.cpp`
  (the mean |log2| luminance difference between captures, blurred), and `perfbaseline.ps1`'s convergence captures.

What already makes a capture's frame the same in every run: the autoload exit counts drawn frames from when the loads are
done, the model installed, the views placed and the scene's graphics pipelines made (a first run after a shader change
draws without them for seconds), see `client/ui.cpp`.

## Principles

- **Checks that need no stored images first.** A check of the renderer against its own references, or against a known
  answer, holds on every driver and survives an intended change of the look; stored images need updating with every such
  change and differ by driver.
- **lavapipe is the reference platform for stored images**, where they are needed: CI's driver, the same everywhere, no
  gpu. References are per driver (KosmicKrisp's and NVIDIA's frames differ in the last bits: precision, filtering,
  subgroup sizes), and every comparison has a tolerance, measured and recorded with its reason.
- **CI's budget**: a lavapipe client takes 14 s warm and 82 s cold (BumpDomes, 320x180, the shaders compiled in the
  first); every check added to CI states what it costs there.
- The codebase's ways: the tools in C++ (`src/tools`, core's file I/O and containers, no python), the scripts in
  PowerShell, capacity limits failing hard.

## The steps, in order

### 1. Repeatable captures

A capture is only comparable if two runs of it give the same image. What can still differ between runs, to be checked
and pinned one by one:

- **Exposure**: auto exposure settles by time; `SPEEDO_AUTO_EXPOSURE=0` with `SPEEDO_EXPOSURE` (or the camera's manual
  mode) fixes it.
- **The sky**: the date defaults to this year's (`environment::CurrentYear`), so the sun and moon move from one run to the
  next; `SPEEDO_YEAR`, `SPEEDO_DAY_OF_YEAR`, `SPEEDO_TIME_OF_DAY` and `SPEEDO_LATITUDE` fix it.
- **Time**: `SPEEDO_TIME_FRAME_SECONDS` advances time by a fixed step per drawn frame instead of the real time between
  frames (whatever animates, the sky's motion, motion blur's shutter).
- **Compute pipelines**: made ahead in chains in the background ("compute pipelines created ahead"); check whether a pass
  can run, or be skipped, before its pipeline exists, and if so wait for them as the graphics pipelines are waited for.
- **Temporal accumulation**: the indirect lighting's probes, surface cache, near field and emitters, temporal
  upscaling's history and jitter, and any other pass that keeps a history. From a cold start, a fixed number of frames at a fixed time step gives
  the same history if everything above is fixed; checked, not assumed.
- **The gpu**: float atomics accumulate in whatever order the threads arrive, and lavapipe runs its work on several
  threads. A run measured twice, bitwise and then within a tolerance, says whether this matters.

The deliverable: one switch that sets all of it for a test capture (an `assettest.ps1 -Capture` mode, or a variable the
client reads), and the measurement that two captures of the same pose agree: per platform (lavapipe on Linux, lavapipe
on Windows, KosmicKrisp), bitwise or to how many levels of 255, recorded here.

### 2. A comparison tool

`src/tools/imagecompare.cpp`, beside `convergence` (which shares its luminance code): two bmps in, measures out, and an
exit code against thresholds given on the command line:

- per pixel: the largest and the mean absolute difference per channel, and the share of pixels past a threshold (a
  small region gone wrong shows here when a mean hides it);
- the mean |log2| luminance difference, blurred and not (`convergence`'s);
- a perceptual measure: NVIDIA's FLIP (BSD-3, C++), if it can come in as a port, else later;
- a difference image (a bmp, amplified) for a person to look at.

Tested on made-up pairs (one pixel off, a shifted edge, noise) so that its numbers mean what they say.

### 3. The renderer against its own references

Pairs of debug views of the same pose in the same run, compared within a tolerance, no stored image:

- traced shadows as the main pass takes them (`DEBUG_VIEW_TRACED_SHADOW`, a ray a pixel filtered over the penumbra's
  width) against `DEBUG_VIEW_TRACED_SHADOW_REFERENCE` (many rays a pixel over the light's disk, unfiltered);
- the emissive surfaces' light as the main pass takes it (`DEBUG_VIEW_EMITTERS`, filtered and accumulated) against
  `DEBUG_VIEW_EMITTERS_REFERENCE` (many independent estimates a traced pixel, accumulated while the camera is still);
- the software BVH against the hardware ray queries (`SPEEDO_RAY_QUERY=0` and `1`): lavapipe has both, so the two trace
  paths give the same image within their noise, a check no gpu platform runs cheaply (macOS's ray queries are opt in);
- temporal upscaling at native resolution against upscaling off (`SPEEDO_UPSCALING=native` and `off`; docs/notes/rendering.md
  records means of 0.05 to 0.15 of 255 between upscaling and none for still renders, the tolerance to start from, and 7.6
  to 8.3 with the sun moving at 600 times real time).

The indirect lighting's checks are step 4: it has no usable reference yet. The reference views take seconds a frame:
captured after a handful of frames, on small scenes (ShadowScene, ManyLights, CausticSpheres, the CornellBox).

### 4. The indirect lighting against a real reference

The user's ask, 2026-11-11: reference checks for the indirect lighting too, finding out how to get good references as part
of the work. The indirect lighting (the probes over the voxel clipmap, the surface cache, the near field, see
docs/notes/indirect-lighting.md) is the part of the frame most likely to be wrong in ways that still look plausible:
light leaking through walls, missing occlusion in corners, energy lost or gained over bounces, a bias of the method that
no amount of settling removes. Its existing references don't serve: the surface reference
(`DEBUG_VIEW_GI_SURFACE_REFERENCE`) is no reference (the user, 2026-10-24: too slow, and it doesn't look like one; seconds
a frame, one bounce on its first frame, noisier than the cache), and the probe reference (`DEBUG_VIEW_GI_REFERENCE`) checks
a probe's gathering from the environment, not the lighting at the surfaces.

**First, a reference worth trusting.** The candidates, to be tried and measured in this order:

- **A progressive path tracer**, a debug view of its own: per pixel, paths from the visible surface through the same
  materials (the BRDF the main pass uses), the same lights (the punctual ones, the emissive surfaces, the environment or the
  sky), over the triangles through the trace paths the renderer already has (the software BVH, and the ray queries where
  the device has them), accumulated while the camera is still, unbiased (no probes, no cache, no clamping but a documented
  one, the bounce count high enough not to matter, with Russian roulette past it). It gives the same quantity as the views it
  is compared with (the indirect diffuse a white surface reflects, as `DEBUG_VIEW_INDIRECT_DIFFUSE`, and the full lit
  image), so that a difference is the indirect lighting's error and nothing else. The likely answer: it shares the scene,
  the materials and the trace code with the renderer, so it costs the least to build and keep in step.
- **The real-time lighting settled**: the same method with its rays, hysteresis and history lengths turned up and run for
  many frames. Cheap, and useful against noise and flicker, but it shares the method's bias, so it can't be the reference
  for leaks or missing occlusion; at most a second, cheaper check.
- **An offline renderer** (Mitsuba, pbrt): independent of the renderer's code, but the scenes, materials and lights would
  need converting with every difference between the two models accounted for; kept as the cross-check of the path tracer
  on one or two scenes, if the path tracer's own validation leaves doubt.

**The reference checked before it is trusted**, against answers known without it:

- **A furnace interior**: inside a closed shape whose walls all emit Le and reflect a diffuse albedo a, the radiance
  everywhere is Le / (1 - a), every bounce included. The path tracer must give it; the indirect lighting's error against it
  is its energy over bounces, directly.
- **A white furnace**: white surfaces under a uniform environment and nothing else (see step 6) come out flat at the
  environment's radiance.
- **A Cornell box** (the quick tier's, from McGuire's archive): its walls' colors bleeding onto each other, against an
  offline renderer's image of the same box, by eye and by the blurred measure.

**Then the checks**, per scene and pose: the indirect diffuse (`DEBUG_VIEW_INDIRECT_DIFFUSE`, and the indirect lighting's
own `DEBUG_VIEW_GI_IRRADIANCE`) against the reference's, by the blurred |log2| luminance (the lighting is low frequency:
blurred, the reference's noise and the drivers' last bits drop out, and what is left is the method's error), and the share
of the image off by more than a stop (a leak or a dark corner is a region, not a mean). The scenes: the CornellBox, a
room lit by the sun through a window (the indoor light all indirect), thin walls between a lit and a dark room (leaks),
salle_de_bain and Sponza's interior; the tolerances from what the indirect lighting gives today, measured, so that the
checks guard what it does now and tighten as it improves.

**Where it runs**: a reference converges slowly, and lavapipe draws slowly (BumpDomes' frames at 320x180 take tenths of a
second), so a path traced reference may take minutes there. Measured first; if it is too slow for CI, the references are
rendered once on a gpu, to many samples, and stored (blurred comparisons tolerate the drivers' differences, so one set
serves every platform), and rendered again when the scene, the materials or the reference itself changes.

### 5. Golden images on lavapipe

Stored captures of chosen poses, compared on CI within their tolerance:

- **First the views without lighting**, which depend on the least: base color, normal, geometry normal, roughness,
  metallic, texcoord, material (`DEBUG_VIEW_BASE_COLOR` to `DEBUG_VIEW_TEXCOORD`). They catch the importers', the
  materials' and the visibility buffer's regressions, and should agree bitwise.
- **Then the lit frames** of the scenes made for a feature (ShadowScene, BlendOrder, BlendInstances, FogHall,
  CausticSpheres, ScatterTexture, VisibilityTiers, ManyLights, ThousandsOfLights, TextureTransformTest), the split screen
  layout, and the sky at fixed dates and times (day, dusk, night with the moon).
- **The workflow**: `assettest.ps1` compares and, on a failure, keeps the capture, the reference and the difference image
  as CI artifacts; an update mode (`-UpdateReferences`) writes new references, and a change that alters the look commits
  them with it, saying why.
- **One lavapipe or two**: Linux takes Ubuntu's, Windows builds the fork's (a different mesa and LLVM). Measured first;
  if they disagree past the tolerance, Linux builds the port's too, so that one set of references serves both.

### 6. Known answers

Scenes whose correct image is known without a reference renderer:

- **A white furnace**: white spheres, rough and smooth, metal and not, in a uniform white environment: a renderer that
  conserves energy draws them flat at the environment's radiance (less only where a BRDF's multiple scattering is
  missing, by a known amount). It catches the BRDF's, the environment prefilter's and the indirect lighting's gains and
  losses. A test model of its own and a constant environment.
- **A grey card**: 18% grey at a known EV100 with a manual exposure comes out at the value the tonemapper's curve gives
  for it.
- **The sky**: the sun's and the moon's directions at fixed dates against an almanac (docs/notes/sky.md checks them by
  hand): the disk's centre in a capture against the expected direction through the camera.
- **The lens**: a grid through a lens prescription against the prescription's own mapping (distortion, vignetting),
  which assettest checks on the cpu but not through the renderer.

### 7. What a cpu driver makes cheap besides

Not visual, and each small on its own; any of them can go in between the steps above:

- **Ray queries on CI**: confirm that the lavapipe clients use them (the client's log line), then CI covers the ray query
  paths, which macOS runs only on request.
- **Synchronization validation** (the validation layer's `validate_sync`) on the CI clients: lavapipe runs work in order,
  which hides a missing barrier a gpu would show.
- **A sanitizer client run** (asan and ubsan) on Linux, headless on lavapipe.
- **More models on CI**: from BumpDomes to a CI subset of the quick tier, chosen by what each costs on lavapipe and the
  time a job may take.

## Open questions, the user's calls

- Where stored references live: in the repository (320x180 bmps are 225 KB each), with git LFS, or with the test
  assets in the binary cache's bucket.
- The CI time each platform's checks may take: no budget set yet (the user, 2026-11-11: build the checks, measure what
  they take, and decide then). So every check records its time on lavapipe, per platform, in the progress below.
- The indirect lighting's references (step 4): which candidate, once tried; and rendered in the run or stored, once what
  they cost on lavapipe is measured.

## Progress

### Step 1, the first day (2026-11-11)

Measured with BumpDomes' dome at 640x360, headless, everything the variables pin pinned (`SPEEDO_AUTO_EXPOSURE=0
SPEEDO_EXPOSURE=0 SPEEDO_YEAR=2026 SPEEDO_DAY_OF_YEAR=172 SPEEDO_TIME_OF_DAY=10:30 SPEEDO_LATITUDE=57.7
SPEEDO_TIME_FRAME_SECONDS=0.0166667 SPEEDO_UI=0`, `SPEEDO_AUTOLOAD_EXIT=30`), captured twice and compared (a throwaway C
comparison: per channel max and mean, the share of pixels past 0, 1, 4 and 16 levels, a difference image):

- **As the client was**, on KosmicKrisp: two warm runs differed in 66% of the pixels, up to 67 levels (0.06% past 16),
  the first run against them in 85%. The differences: noise everywhere, the edges, and broad patches of the ground.
- **What made them differ**: the frames' sequences (the jitter's Halton point, `Views::myJitterFrame`; each pass's frame
  count, which seeds its noise and candidates; the indirect lighting's clocks) and the histories over frames all count
  from the renderer's first frame, while the capture comes 30 frames after the model is in and its pipelines made, which
  varies by a frame or more; the ui thread asks for the capture and the draw thread takes whichever frame it is on; and
  the indirect lighting's blend follows the real time between frames (`myGiTimeBased`), not `SPEEDO_TIME_FRAME_SECONDS`.
  Two captures at the same jitter frame still differed (57% of pixels, up to 34 levels): the histories' lengths differed.
  Not the compute pipelines: those made ahead are waited for before the scene is ready, and one missing at a bind is made
  then, not skipped.
- **`SPEEDO_REPEATABLE=1`** (new): when the autoload exit starts counting, the draw thread restarts the frames' sequences
  and histories (`Scene`, `Renderer` and `Views::RestartSequences`: the counts zeroed, every history invalid, the
  indirect lighting made again as for a new scene) at the start of its next frame, and captures the
  `SPEEDO_AUTOLOAD_EXIT`-th frame from there itself; the indirect lighting's blend takes `SPEEDO_TIME_FRAME_SECONDS`
  where set. On KosmicKrisp three runs then differed by at most 11 to 13 levels (rather than 50 to 67), with any one of
  the indirect lighting and temporal upscaling off by at most 1 level, and the base color view (`SPEEDO_DEBUG_VIEW=1`)
  bitwise identical.
- **What is left on KosmicKrisp**: 1 level in most pixels, from the ambient light (the indirect view, `SPEEDO_DEBUG_VIEW=13`,
  46% of pixels; the direct light's 6 pixels), the same with a fixed panorama for the environment (neutral.hdr) rather
  than the sky, in its diffuse and specular halves alike: the environment's lighting differs slightly as a whole from
  run to run. Synchronization validation (`SPEEDO_VALIDATE_SYNC=1`) reports no hazard. Not yet found.
- **On lavapipe** (the mesa port's, macOS): 96% of the pixels identical in two runs, the rest within 10 levels, on the
  domes' shaded halves: the indirect lighting's (off, everything within 1 level). Its voxels' atomics are maxima, which
  don't depend on order; likely the gpu's statistics the cpu reads back to steer it (the surface cache's growth, the
  capped rays), read from whichever frame the gpu has finished, which varies: a loop to pin next. lavapipe against
  KosmicKrisp: a different image (mean 13 to 15 levels in red and blue), so references per driver, as planned.

- **The gpu's statistics** are read a frame index's `SHADER_TYPES_FRAME_COUNT` frames later (after its fence), a fixed
  latency, but for that many frames after the restart they were of the frames before it: skipped now, and what they
  steer (the visibility buffer's sorting, the surface cache's record pool) back at its defaults. KosmicKrisp then 36 to
  52% of the pixels off by a level and at most 7 to 11 (from 86 to 95% and 11 to 13); lavapipe barely moved (2.8 to 3.1%,
  at most 7 to 9): not what is left of the indirect lighting's differences there.

Next: what is left of the indirect lighting's (lavapipe: a few percent of the pixels, within 10 levels) and
KosmicKrisp's ambient light, or tolerances for them (step 2's tool measures what a tolerance has to allow); then the
measurement on Linux's and Windows' lavapipe.

- **Waiting until everything is done** (the user's call, 2026-11-11: "wait until all are done before screenshotting"):
  the autoload exit waited for the model's install and the graphics pipelines but not for the other installs' binds, so
  a run that imported its panorama (neutral.hdr, cold) captured its first frames lit by the sky, the panorama bound a
  few frames later (TransitionThenBind, after its uploads): 24 to 33 levels apart on average from a warm run's. The scene
  counts the installs a load queued and not yet bound (`Scene::HasPendingInstalls`: models', images', environments'), and
  the count starts once there are none and the pipelines are made. A cold run against a warm one then: at most 40
  levels, a mean of 0.24, as two warm runs.

### Step 4, the furnace interior (2026-11-11)

The first known answer, before a path tracer: `scripts/test-assets/furnace.ps1` writes
`resources/test-models/gltf/FurnaceInterior.gltf` (a closed cube 4 m a side, its walls facing inward, albedo 0.5,
emission 1), inside which the radiance is Le / (1 - a) everywhere: twice the emission, +1 stop. The `furnace` check
renders it from its center, read linearly (the linear tonemapper, `SPEEDO_EXPOSURE=-3`, no upscaling's history), the
indirect lighting's frame against its emission alone (`SPEEDO_GI=0 SPEEDO_EMISSIVE_LIGHTS=0`), and holds the ratio at
what it gives (`imagecompare --expect-ratio 0.88 --ratio 0.03`: the ratio's threshold either way of an expected value).

Its first run found a portability bug. In the check KosmicKrisp gave +0.866 stops and lavapipe +0.730; the probes alone
(`SPEEDO_EMISSIVE_LIGHTS=0`) +0.82 and +0.53, the emitters as lights alone +0.53 on both (one bounce, 1 + a, is +0.58).
The debug views on both drivers localized it: the voxels' albedo and normals, the clips, the probes' states and the
probe reference the same; the voxels' radiance as the probes' rays see it (`gi-voxel-world`) 0.9 on KosmicKrisp (0.5
times its irradiance, 1.79) and 0 on lavapipe, whose irradiance was 1.09, the emission's alone. A diagnostic build
coloring ComputeGiLight's branches showed lavapipe's voxels keeping the radiance of the frames before the bounce, the
light pass skipping them: a voxel's thread skips its lighting when its block's flag bit 0 (anything near it last frame)
is clear, and each lit voxel stored its block's stamp without the flag bits, so any of the block's voxels that ran after
one had stored skipped theirs. A block's voxels are in 16 groups (a row of 4 along x each); lavapipe runs the groups one
after another and lit a row of each block, a gpu's concurrent waves mostly read before any stored, but not always
(KosmicKrisp had blocks unlit too). Fixed by keeping bits 0 to 3 in the store (ComputeGiFlags makes them again right
after): +0.881 on both drivers, 0.0002 apart, the voxels' radiance and the irradiance the same to the level, every
check passing on both.

What is left: about 8% of the light missing at 256 frames (0.12 stops), settling slowly; the path tracer is to tell the
method's bias from that, the furnace being its first check.

### Step 3, the first finding (2026-11-11)

The traced shadows against their reference (`DEBUG_VIEW_TRACED_SHADOW` and `_REFERENCE`) on ShadowScene: by the sky's
sun (10:30, 18:30) nothing soft in view, the two the same; lit by neutral.hdr (its softbox a dominant light, the first
traced) the reference had the boxes' shadows on the floor and the traced view none, nor had the frame. With
`SPEEDO_TRACED_SHADOWS=all` they were there, and the views differed only along the shadows' edges: the atlas had no
shadows for the light, and the default mode traces only where the atlas isn't sure. The cause, after the user's hint
(the pipelines made asynchronously): the shadow cache kept the views of a frame that had cleared its tiles without
drawing them, its pipelines still being made, for as long as nothing changed (see docs/notes/rendering.md, "Shadows").
Fixed: the default frame against every pixel traced 0.035% of the pixels past 16 levels, the traced view against the
reference 0.004 stops blurred, its edges alone. A bug of the normal frame, not just of the checks.

### Step 3, the checks (2026-11-11)

`scripts/visualtest.ps1 -Preset <preset>` runs them: each a scene rendered twice, headless and repeatable, as a
reference and a capture, compared by `imagecompare` against thresholds a few times what was measured (each check's
comment has its measures). `imagecompare` gained the mean luminance ratio (`--ratio`, in stops, signed: light gained or
lost), which tells light spread differently (a filter's, a reference's blocks) from light gained or lost, where the
mean |log2| difference counts both. The checks and their measures on KosmicKrisp:

- `traced-shadows`: the traced view against its reference (ShadowScene, neutral.hdr): 0.004 stops blurred, -0.002 of
  light. It can't tell an atlas gone wrong: both views trace only where the atlas isn't sure, and with the shadow cache's
  fix taken out they came out the same.
- `shadow-atlas`: the frame in the default mode against every pixel traced (`SPEEDO_TRACED_SHADOWS=all`): 0.0002 stops
  blurred, +0.0001 of light. With the fix taken out it fails, cold and warm (0.94% of the pixels past 16 levels, +0.009
  stops of light: the shadows missing).
- `emitters`: the emitters' view against its reference (the CornellBox's lamps, 32 frames): 0.085 stops blurred over 24
  pixels, +0.002 of light; the same light, spread differently. Both views dim alike over longer runs (128 to 512 frames,
  0.0287 to 0.0268 of linear luminance in the lower half): to look into.
- `upscaling`: native temporal upscaling against none (SheenChair, in the quick tier's assets, which CI fetches; it was
  DamagedHelmet, which isn't): 0.48% of the pixels past 16 levels (edges and fine detail), 0.005 stops blurred, -0.004
  of light, on KosmicKrisp and lavapipe alike.
- `ray-queries`: the hardware ray queries against the software BVH (the lit frame of ShadowScene with neutral.hdr), run
  on lavapipe alone (skipped on KosmicKrisp, where ray query clients wait for the user's go-ahead, and where the device
  has none, both runs then tracing the BVHs): 0.0004% of the pixels past 16 levels, 0.0001 stops blurred, -0.00002 of
  light, within lavapipe's own run to run differences.

A run takes 38 s on the M4 Pro's KosmicKrisp from a new work dir (the first client's shader compile 30 of them), 9 s
warm; 151 s on its lavapipe (the mesa port's), all five passing, the first four as on KosmicKrisp although the two
drivers' frames differ (each check compares a renderer with itself). CI runs them on Linux's and Windows' lavapipe
(Windows' driver registered in a step of its own, the elevated loader's), test assets from `SPEEDO_TEST_ASSETS` (the
script maps `test-assets/` there); their times there are to be measured.

### Step 3, validated (2026-11-12)

`visualtest.ps1 -Validate` runs the clients under the validation layer, with assettest's settings (`VK_LAYER_PATH` the
install tree's unless set, the layer's own log beside each client's) and its problem patterns (a validation message, a
failed load, a sanitizer report): any fails the check, the first five printed. A debug client loads the layer itself,
so there the switch only points `VK_LAYER_PATH` at it. The first validated run on lavapipe failed the furnace on
VUID-VkWriteDescriptorSet-descriptorType-00333: the GI radiance buffer, three parts of a uint2 per voxel, was 144 MiB at
the default resolution, past lavapipe's `maxStorageBufferRange` of 128 MiB; it is three buffers now (see
docs/notes/indirect-lighting.md). BumpDomes' validated client runs on CI never met it. With that, all six pass validated
on the M4 Pro's lavapipe in 276 s from a new work dir (the furnace 95 s, the ray queries 78 s), and five on its
KosmicKrisp in 72 s (54 of them the first client's, with the shader compile), the measures as before (the furnace
+0.881 stops). CI's visual checks took `-Validate`, and its client checks (BumpDomes, `assettest.ps1 -ClientOnly`) were
dropped (the user's call): the visual checks load and draw models the same way, validated, and more of them. CI's first
validated run (2026-11-12) passed all six on both runners' lavapipe, the measures as on the M4 Pro: on Linux the step
took 617 s (traced-shadows 100 s with the shader compile, shadow-atlas 26, emitters 63, upscaling 75, furnace 254,
ray-queries 95), on Windows 1223 s (182, 105, 158, 174, 447, 154). No budget yet; the furnace, 256 frames, is the most
of it on both. macOS's job runs them too since 2026-11-13 (the user's ask), on the mesa port's lavapipe beside
KosmicKrisp in the host package: all six pass on the M4 Pro's in 305 s from a new work dir (69, 10, 25, 28, 95, 74), the
measures as before.

### Step 2 (2026-11-11)

`src/tools/imagecompare.cpp` (`imagecompare <reference.bmp> <capture.bmp>`): per channel the largest and the mean
difference, the share of the pixels past 0, 1, 4 and 16 levels and where they are, the mean |log2| luminance difference
sharp and blurred (`--blur`, 8 pixels by default), each a threshold (`--max`, `--mean`, `--over <levels>:<percent>`,
`--log`, `--log-blurred`: exit 1 past any), and the differences as an image (`--diff`, 16 times). It shares
`src/tools/capture.h` with `convergence` (the captures read and written through core's files, their luminance, the box
blur, the log difference), which used to have its own. `--self-test` checks the measures on made-up pairs: the same
image, a pixel off by 50 in red (and where), an edge moved a pixel (one column, which blurring mostly hides), a level of
noise, twice the light (a stop, sharp and blurred). Not yet FLIP. On step 1's repeatable pairs: KosmicKrisp's at most 7
levels, 0.23% past 1, 0.0013 stops blurred; lavapipe's at most 9, 0.10% past 1, 0.00004 stops blurred: both within
`--max 16 --over 1:1`, and lavapipe against KosmicKrisp far past it.
