# Visual testing: how speedo checks what the client draws

Running the client headless proves that it runs, not that it draws the right thing. Visual testing checks the output: captures are made repeatable (`SPEEDO_REPEATABLE` and the variables that pin the rest), `imagecompare` measures how far two captures differ, and `scripts/visualtest.ps1` runs checks that compare the renderer with its own reference view, another setting of itself, a known answer or a path traced image. Most checks need no stored images, so they hold on every driver and survive an intended change of the look. CI runs them on lavapipe, WARP and Metal.

## Running the client for a test

Test and experiment runs are headless: `SPEEDO_HEADLESS=1 SPEEDO_AUTOLOAD_EXIT=<frames> SPEEDO_CAPTURE=<file.bmp> SPEEDO_UI=0 SPEEDO_WINDOW_SIZE=<w>x<h> SPEEDO_CAMERA=x,y,z,pitch,yaw`, from the repository root with a scratch profile (`-u <dir>`). Keep the user folder between runs (`temp/user`, one per client running at once): models, images and environments are cached in it (docs/notes/assets.md, "Caches"), so a new user folder imports every asset again. The scripts make a new temporary `-Work` unless given one; pass a fixed one (`temp/<script>`) to keep their caches. They need no display, so a locked or dark screen doesn't fail them, and the test scripts set `SPEEDO_TRACY=0` so that a profile build's client doesn't buffer Tracy's events. A run is windowed (`SPEEDO_BACKGROUND=1`, which keeps the window behind the others without taking the focus, and `SPEEDO_PRESENT_MODE=fifo`) only where the test needs a window: presentation, fullscreen, the ui's input, and fifo frame times; then the screen must be on. A report of an artifact starts from Edit > Copy Scene's text (the camera and every changed setting as these variables), which Paste Scene and the autoloads take back.

## Repeatable captures

`SPEEDO_CAPTURE=<file.bmp>` saves the frame that `SPEEDO_AUTOLOAD_EXIT=<frames>` ends on. `visualtest.ps1` pins the rest of what varies between runs:

- `SPEEDO_REPEATABLE=1`: when the exit's count starts, the draw thread restarts the frames' sequences and histories (jitter, noise, temporal histories, the indirect lighting) and the animation, and captures the count's last frame itself.
- `SPEEDO_AUTO_EXPOSURE=0` and `SPEEDO_EXPOSURE=0`: a manual exposure.
- `SPEEDO_YEAR`, `SPEEDO_DAY_OF_YEAR`, `SPEEDO_TIME_OF_DAY`, `SPEEDO_LATITUDE`: the sun and moon, which otherwise follow today's date.
- `SPEEDO_TIME_FRAME_SECONDS`: a fixed time step per frame instead of real time.
- `SPEEDO_HEADLESS=1`, `SPEEDO_UI=0`, `SPEEDO_WINDOW_SIZE`: no window, no ui, a fixed size.

The count starts only once every queued install is bound and the scene's pipelines are made, so cold and warm runs match. `SPEEDO_CAPTURE_COUNT` and `SPEEDO_CAPTURE_STRIDE` save several frames of the count, the same every run. Repeatable is not bitwise identical: float atomics and driver scheduling leave small differences, so every comparison has a tolerance and references are per driver.

## imagecompare

`build/<preset>/imagecompare <reference> <capture>` (`src/tools/imagecompare.cpp`, sharing `capture.h` with `convergence`) takes two images of one size; the reference may be a png. It measures:

- the largest and mean per-channel difference, in levels of 255;
- the share of pixels past 0, 1, 4 and 16 levels, and the bounding box of the differing pixels;
- the mean |log2| luminance difference in stops, sharp and blurred over `--blur` pixels (default 8: the lighting's error rather than its noise);
- the mean luminance ratio in stops, signed: light gained or lost, as against spread differently.

Thresholds fail it with exit 1 (2 on an error): `--max`, `--mean`, `--over <levels>:<percent>`, `--log`, `--log-blurred`, and `--ratio` around `--expect-ratio` (default 0). `--diff` writes the differences amplified 16 times, `--fit` scales a reference of another size to the capture's, and `--self-test` checks the measures on made-up pairs.

## scripts/visualtest.ps1

`pwsh scripts/visualtest.ps1 -Preset <preset> [-Work <dir>] [-Only <names>] [-Size WxH] [-TimeoutSeconds <n>] [-Validate]`. Each check runs the client once or twice, compares, and keeps captures, logs and `differences.bmp` under `<work>/<check>`; `-Size` is the window size and `-TimeoutSeconds` how long each client run gets. A client crash, timeout, load failure, assert, validation message or sanitizer report fails it too. Thresholds are a few times what was measured when the check was made: a check whose measures move should be looked at, not loosened. `-Validate` turns on the validation layer (Metal's api validation, D3D12's debug layer). A Vulkan client needs a driver; on macOS the mesa port's KosmicKrisp unless `VK_DRIVER_FILES` names another.

| Check | Compares | Scene |
|---|---|---|
| `traced-shadows` | traced shadow view against its many ray reference view | ShadowScene, `neutral.hdr` |
| `shadow-atlas` | default shadow mode against every pixel traced | ShadowScene, `neutral.hdr` |
| `emitters` | emissive lights' view against its reference view | CornellBox |
| `upscaling` | native temporal upscaling against off | SheenChair |
| `furnace` | indirect lighting against the emission alone, a known answer | `FurnaceInterior.gltf` |
| `ray-queries` | hardware ray queries against the software BVH | ShadowScene, `neutral.hdr` |
| `research-cornell-box`, `research-material-testball` | one run against a path traced Cycles image | glTF research set |

- **Own references.** The first four need no stored image. `traced-shadows` can't catch an atlas gone wrong, since both views trace only where the atlas is unsure; `shadow-atlas` can.
- **The furnace.** `scripts/test-assets/furnace.ps1` writes a closed cube of emitting diffuse walls, where the radiance is Le / (1 - a) everywhere (+1 stop at a = 0.5). The check renders it from its centre, linearly, and holds the ratio to the emission alone at what the renderer gives, `--expect-ratio 0.93`: some light is still missing, and the check tightens as that improves.
- **Ray queries.** Both trace paths answer the same rays, so the images agree within noise. Skipped on KosmicKrisp, with `SPEEDO_RAY_QUERY=0`, on Metal unless `SPEEDO_RAY_QUERY=1` (Metal's ray tracing restarts the gpu now and then), and when the log shows no ray query structures.
- **Research checks.** They run in the conditions of the scene's `conversion.yaml` against the set's reference, scaled with `--fit`, and measure the renderer's approximation, held where it is rather than at zero (docs/research-scenes-plan.md).

`assettest.ps1` covers what needs no frames: importer decodes, the BVH against brute force, lens prescriptions, and with `-Client` validated client runs of each model.

## scripts/researchcompare.ps1

`scripts/researchcompare.ps1 -Preset <preset> [-Work <dir>]` renders each scene of the glTF research set as the client opens it, in the conditions of its `conversion.yaml` (docs/research-scenes-plan.md), headless, at the reference's size with the file's first camera, the camera's own effects off. It measures each capture against the reference with `imagecompare` and writes a sheet of the two and their differences per scene, an overview of every pair and a table of the measures, the scenes most unlike their references first, under `-Review` (default `temp/review/research`). It passes or fails nothing: it maps what the renderer approximates, where `visualtest.ps1`'s research checks hold it. Use it to see the research set's state after a change to lighting or materials.

- `-Only <scenes>` renders those alone; `-Kind core` renders the files without the material extensions (default `extended`); `-Units <scene=unit>` gives a scene's unit where it is not meters.
- `-Set <name=value>` adds variables to every run, for an experiment; `-Validate` loads the validation layer.
- `-Frames` (default 1500) and `-TimeoutSeconds` (default 900) bound a run; `-Jobs` (default 4) clients run at once, each with its own user directory under `-Work`.
- `-Timings` times each scene after the captures, one client at a time, at `-TimingSize` (default 2560x1440) for `-TimingFrames` frames: the frame time and the costliest passes' gpu times, in the table and the overview. Not with `-Validate`.
- `-Chart <path>` also writes the overview as a JPEG, for people: each pair labelled (the reference's renderer, speedo's graphics api), how much brighter or darker speedo's image is and the frame time, under a line naming the operating system, graphics api, gpu (`-Device`, else the system's name of it) and how rays were traced. Beside it, `<path>` with `.md` holds the measures in full (both log differences, the light's ratio, the mean difference, frame times and costliest passes): `docs/research-scenes/overview.jpg` and `overview.md`, by which the plan tracks its progress.
- `-Reuse` runs no client: it measures, sheets and charts what `-Work` holds from an earlier run. `-KeepCaptures` captures only the scenes without a capture in `-Work` (from an earlier run, or put there at the reference's size) and still times every scene.
- The images are made by `imagesheet` (docs/notes/layering.md, "Tools"), so the script runs on every platform.

## CI

Each platform's job runs `visualtest.ps1 -Validate` on lavapipe, mesa's cpu Vulkan driver. A Windows job on D3D12 under WARP runs `traced-shadows`, `shadow-atlas`, `emitters` and `ray-queries`. The macOS job also runs the checks on Metal on the runner's gpu, without the ray query check. Details: docs/notes/build.md, "CI".

## Open steps

- **Auto exposure repeatability.** Auto exposure meters differently between runs even with `SPEEDO_REPEATABLE=1`, probably from the histogram read back from whichever frame the gpu has finished, and the adaptation's time.
- **Small differences left** in the indirect lighting on lavapipe and KosmicKrisp, not understood; tolerances absorb them.
- **The indirect lighting against a real reference**: a progressive path tracer as a debug view, validated against known answers, then compared by the blurred log difference and the share of the image off by more than a stop. Whether references are rendered in the run or stored depends on their cost on lavapipe.
- **Golden images** on lavapipe in CI, starting with the lighting free debug views, with an update mode. Open: where stored references live, and whether one set serves every platform.
- **More known answers**: a white furnace, a grey card, the sun's direction against an almanac, a lens grid through the renderer.
- **FLIP** as a perceptual measure, if it can come in as a port.
- **CI additions**: synchronization validation, a sanitizer client run on Linux, more models. No CI time budget is set yet.
