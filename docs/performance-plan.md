# Performance: how frame time and the lighting's convergence are measured

The client is meant to grow into a cross-platform game platform, so performance work favours general methods that scale with content and run everywhere, with a platform's exceptions named for what it lacks. Two things are measured: what each feature and pass costs in frame time, and how many frames and seconds the indirect lighting takes to settle from a cold start. `scripts/perfbaseline.ps1` runs both at fixed scene poses and `src/tools/convergence.cpp` analyses the settling. The two are one problem, since the lighting settles over frames and a faster frame settles sooner.

## Measuring frame time

The measure is the draw thread's frame time, printed at exit with `SPEEDO_GPU_TIMINGS=1`: the median over half second windows, in fifo present mode. Where the draw thread waits on its fences it is the gpu's time. On KosmicKrisp this is the measure to use, since its gpu timestamps miss compute passes and the `Frame` scope undercounts a compute heavy frame.

- **Warm pipelines.** The first run after a shader change draws with generic pipelines and is slower. Measure after the lighting's warm-up too: the surface cache traces more rays for its first frames, which the client times apart as "frame time warming up".
- **Interleave the runs compared**, the default first and last. The gpu's clock drifts between runs and sessions, so only numbers from one session compare; a session far over its usual baseline is a machine problem.
- **Windowed, not headless**: runs use `SPEEDO_BACKGROUND=1`, fifo and no ui. On macOS the script keeps the display and system awake.

A feature's or pass's cost is the frame time it frees when left out. A pass can decide others' work (GI Light and GI Flags gate the passes after them, the shadow atlas feeds the classification), so its row includes that.

## scripts/perfbaseline.ps1

`pwsh scripts/perfbaseline.ps1 -Preset <preset> [-Scenes bistro,sanmiguel,sponza] [-Features] [-Passes] [-Convergence] [-Size WxH] [-Work <dir>] [-Frames <n>] [-ConvergenceFrames <n>] [-Stride <n>] [-KeepCaptures]`; with no mode switch it runs all three. Tables go to `<work>/baseline.md`. `-Frames` is how many frames each feature and pass run lasts, `-ConvergenceFrames` how many the convergence run draws. San Miguel loads from its extracted archive, so open `San_Miguel.zip` in the client once first.

- **`-Features`**: everything on, each feature off in turn (`SPEEDO_GI`, `SPEEDO_GI_NEAR`, `SPEEDO_GI_CACHE`, `SPEEDO_EMISSIVE_LIGHTS`, `SPEEDO_TRACED_SHADOWS`, `SPEEDO_SHADOWS` set to 0), all of them off, and the sun moving (`SPEEDO_TIME_SPEED`).
- **`-Passes`**: one frame graph pass left out at a time with `SPEEDO_SKIP_PASSES` (the GI pass by its steps). The culls and pre-passes stay in, since leaving them out changes what everything after them draws.
- **`-Convergence`**: from a cold start at the pose, still, at the exposure auto exposure settled on (it settles by time and would be measured with the lighting). The pose is repeated in a file for `SPEEDO_CAMERA_PATH=@<file>`, every `-Stride`-th frame is captured with `SPEEDO_CAPTURE_PATH` and `SPEEDO_CAPTURE_PATH_STRIDE`, then `convergence` runs; captures are deleted unless `-KeepCaptures`.

## convergence

`build/<preset>/convergence <dir> [--settled <n>] [--blur <pixels>] [--frame-ms <ms>] [--warm-ms <ms> --warm-frames <n>]`. The settled image is the mean of the last captures. Each capture's difference from it is the mean |log2| luminance difference, blurred over `--blur` pixels (default 24: the lighting's error, not its grain). The noise is the last captures' own difference from the settled image: a difference that stays near it is noise, not unsettled light. It prints the curve, the noise, and the frames (and seconds, given `--frame-ms`, with the warm-up at its own time) after which the difference stays under 0.1, 0.05 and twice the noise.

## Open steps

- **Convergence**: blend rates and history lengths as times with a floor in samples, so settling doesn't slow with the frame rate; more rays after a reset or in a new area; a shorter chain of delays; a warm start saved per scene and time of day; static voxels relit over frames when the sun moves. The surface cache's tail is the main item. Still to measure: settling after a change, a camera jump and a time of day change.
- **Cheaper rays**: shadow maps of static casters cached (virtual shadow maps later), and on the software path budgets driven by the frame time and fewer rays for settled probes.
- **Fewer pixels**: the upscaler standing in for the traced shadows', near field's and emitters' own temporal filters; DLSS and MetalFX behind the same interface.
- **Rich worlds**: instances of models, streaming with level of detail, async compute, recording on several threads, quality presets aimed at a frame time.
- **Desktop gpu baselines**, with Nsight for per dispatch times.
