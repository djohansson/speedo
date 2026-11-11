# Visual testing plan: what the client draws, checked

Since 2026-11-11 CI runs the client itself, headless on lavapipe (mesa's Vulkan driver on the cpu: Ubuntu's on Linux, the
mesa port's on Windows, see "CI" in docs/notes/build.md). What those runs check is that the client runs, not what it
draws: a model that comes out black or garbled passes as long as nothing crashes or reports an error. This plan adds checks
of the output, in an order where each step is useful on its own, and the non-visual checks a cpu driver makes cheap.

The user's ask, 2026-11-11: write the plan down and start once the lavapipe CI work is on master.

## What is checked today

- **Client runs** (`assettest.ps1 -Client`, the quick tier locally, BumpDomes on CI): each model loaded, uploaded and
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

Nothing yet: the plan starts once the lavapipe CI work (the `ci-lavapipe` branch) is on master.
