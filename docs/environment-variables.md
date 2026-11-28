# Environment variables

The client reads these at startup to start in a known state (what to load, the camera, every View setting, a capture and an exit after so many frames, the window or none) and to tune and diagnose the renderer. The scripts and the build read a few of their own, listed at the end.

Edit > Copy Scene (`Ctrl/Cmd+Shift+C`) writes the current view as these variables, and Paste Scene (`Ctrl/Cmd+Shift+V`) applies them to a running client: the easiest way to get a set of them. For example, this loads Sponza, places the camera, renders headless without the interface, and saves the 200th frame:

```sh
SPEEDO_AUTOLOAD_MODEL=test-assets/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf SPEEDO_CAMERA=6.14,4.28,-2.41,-18.4,-104.6 SPEEDO_TIME_OF_DAY=14.5 SPEEDO_HEADLESS=1 SPEEDO_UI=0 SPEEDO_WINDOW_SIZE=1280x720 SPEEDO_AUTOLOAD_EXIT=200 SPEEDO_CAPTURE=sponza.bmp ./build/arm64-osx-clang-profile/client
```

On macOS from a checkout, set `VK_DRIVER_FILES` to KosmicKrisp first (see the [README](../README.md#3-run-the-client)).

## Contents

- [How they are read](#how-they-are-read)
- The client
  - [Loading at startup](#loading-at-startup)
  - [The file browser](#the-file-browser)
  - [Test assets](#test-assets)
  - [The camera and views](#the-camera-and-views)
  - [Captures and exiting](#captures-and-exiting)
  - [The window and presentation](#the-window-and-presentation)
  - [The interface](#the-interface)
  - [Time and animation](#time-and-animation)
  - [The camera and lens](#the-camera-and-lens)
  - [Exposure and tonemapping](#exposure-and-tonemapping)
  - [The environment, sky and time](#the-environment-sky-and-time)
  - [Lighting, shadows and ray tracing](#lighting-shadows-and-ray-tracing)
  - [Indirect lighting](#indirect-lighting)
  - [Emissive surfaces](#emissive-surfaces)
  - [Culling and drawing](#culling-and-drawing)
  - [Diagnostics](#diagnostics)
  - [The gpu and drivers](#the-gpu-and-drivers)
- Outside the client
  - [The scripts and the build](#the-scripts-and-the-build)
  - [The Vulkan loader](#the-vulkan-loader)

## How they are read

- **Scene** marks a variable that is part of the scene's state: Copy Scene writes it (when it differs from the default), Paste Scene applies it and resets each scene variable the text leaves out. The View menu changes these while the client runs.
- Scene variables: an empty value counts as unset; names are case-sensitive; a value that isn't understood prints `NAME=value: not understood` and is ignored; for a switch, `0` is off and anything else on; numbers are clamped to the range given, and non-numeric text reads as 0.
- The other variables parse their own way, as their rows say. Several turn on only with exactly `1`.
- The renderer's tuning (most of the indirect lighting's, the emissive surfaces', culling) is read once when the renderer is created and is not in the View menu.
- Paths to load (`SPEEDO_AUTOLOAD_MODEL`, `_ENVIRONMENT`, `_IMAGE`, `SPEEDO_BROWSE`) are absolute or relative to the resource directory (`resources/` in a checkout, or `-r <dir>`). Output paths (`SPEEDO_CAPTURE`, `SPEEDO_CAPTURE_PATH`, `SPEEDO_CAMERA_PATH=@file`, `SPEEDO_TEST_ASSETS`) are relative to the working directory.
- Nothing is saved between runs except the unit of an autoloaded model, which the profile remembers: use a scratch profile (`-u <dir>`) for tests.

## Loading at startup

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_AUTOLOAD_MODEL` | `.gltf`, `.glb`, `.obj`, a folder or zip of models, or files separated by `;`. None. | Scene. Loads them as File > Open... does. |
| `SPEEDO_AUTOLOAD_SCENE` | glTF scene index. The file's default. | Scene. Loads that scene of a single glTF. |
| `SPEEDO_AUTOLOAD_VARIANT` | Material variant index. Default materials. | Scene. Loads one model with that `KHR_materials_variants` variant. |
| `SPEEDO_AUTOLOAD_VARIANTS` | `all`, or indices and `default` separated by commas. None. | Scene. Loads a copy of the model per variant. |
| `SPEEDO_AUTOLOAD_ARRANGEMENT` | `side-by-side`, `as-authored`, `behind-each-other`. Side by side for `;` lists, behind each other for variants. | Scene. How several files or variant copies are arranged. |
| `SPEEDO_GROUND_PLANE` | `0` off. On. | Scene. A ground plane under the autoloaded model. |
| `SPEEDO_UNITS` | `m`, `cm`, `mm`, `in`, `ft` (or full words). The profile's, else a guess. | Scene. The model's unit; wins over `SPEEDO_METERS_PER_UNIT`. |
| `SPEEDO_METERS_PER_UNIT` | Meters, at least 0.000001. As `SPEEDO_UNITS`. | Scene. A unit of any size. |
| `SPEEDO_AUTOLOAD_ENVIRONMENT` | A `.hdr` panorama, or `uniform:<r>,<g>,<b>`. The physical sky. | Scene. Lights the scene with it (center column is +X, three-quarter column +Z). |
| `SPEEDO_AUTOLOAD_IMAGE` | An image file. None. | Loads it onto the default material. |

## The file browser

`SPEEDO_BROWSE` opens File > Open... at startup; the others set its boxes and what it does once it lists the folder.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_BROWSE` | A folder. Closed (opens in the resource directory). | Opens the file browser there at startup. |
| `SPEEDO_BROWSE_ENVIRONMENT` | Exactly `1` on. Off. | Opens the environment browser instead: every panorama below the folder. |
| `SPEEDO_BROWSE_RECURSIVE` | `0` off. On. | Includes subfolders. |
| `SPEEDO_BROWSE_KINDS` | Any of `models`, `archives`, `images`, `panoramas`. Models and archives. | File kinds listed. |
| `SPEEDO_BROWSE_GROUND` | `0` off. On. | Whether opened models stand on a ground plane. |
| `SPEEDO_BROWSE_SELECT` | Entries as listed, separated by `;`. None. | Selects them once listed, previewing the last. |
| `SPEEDO_BROWSE_OPEN` | `as-authored`, else side by side. None. | Opens the selection once listed, as the Open button does. |
| `SPEEDO_BROWSE_OPEN_FRAME_COUNT` | Frames. 0. | Frames to wait after the preview starts loading before opening, to test giving up a preview. |

## Test assets

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_TEST_ASSETS` | A folder. `resources/test-assets` in a checkout (or under `-r`), else `test-assets` in the user's data. | Where test assets live: the download target and where File > Open... starts if they aren't under the resource directory. `fetch-test-assets.ps1` and `visualtest.ps1` read it too. |
| `SPEEDO_DOWNLOAD_TEST_ASSETS` | `offer`, `quick`, `full`. First launch offers. | `offer` opens the Test Assets dialog; `quick` or `full` downloads that set, prints a summary and exits. |

## The camera and views

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_CAMERA` | `x,y,z,pitch,yaw` (scene units, degrees), or `frame`. The model framed, or its first camera. | Scene (Paste Scene needs it). Places every view after the autoloads. |
| `SPEEDO_LAYOUT` | `1x1`, `1x2`, `2x1`, `2x2`. `1x1`. | Scene. The split screen's views. |
| `SPEEDO_CAMERA_VIEWS` | Poses separated by `;` for views 1 to 3, or `around`. `SPEEDO_CAMERA` places all. | Scene. Places the other views (needs a pose in `SPEEDO_CAMERA`); `around` circles the scene's center. |
| `SPEEDO_CAMERA_PATH` | Poses separated by `;` or new lines, or `@<file>`. None. | One pose per drawn frame, then holds the last. The autoload exit waits for it. |
| `SPEEDO_CAMERA_TURN` | Degrees per second. 0. | Turns every view about the vertical axis continuously. |
| `SPEEDO_CAMERA_WALK` | Units per second. 0. | Moves every view forward continuously; combines with turn. |
| `SPEEDO_PASTE_SCENE_FRAME` | Frame count. Never. | Pastes the clipboard's pose at that frame, to test Paste Scene. |
| `SPEEDO_COPY_SCENE_FRAME` | Frame count (of those `SPEEDO_AUTOLOAD_EXIT` counts, if set). Never. | Copies the pose at that frame, as Copy Scene. |

## Captures and exiting

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_AUTOLOAD_EXIT` | Drawn frames. Keeps running. | Exits that many frames after loads are installed, pipelines made and any camera path finished (gives up on a model after 600 frames). Skips the test assets offer. |
| `SPEEDO_CAPTURE` | A `.bmp` path, folder must exist. None. | Needs `SPEEDO_AUTOLOAD_EXIT`. Saves the exit frame (with the interface unless `SPEEDO_UI=0`). |
| `SPEEDO_CAPTURE_COUNT` | At least 1. 1. | Needs `SPEEDO_REPEATABLE`. Also saves the last n frames as `<capture>-<frame>.bmp`. |
| `SPEEDO_CAPTURE_STRIDE` | At least 1. 1. | With the count, saves every n-th frame back from the last. |
| `SPEEDO_REPEATABLE` | Not `0` on. Off. | Only with `SPEEDO_AUTOLOAD_EXIT`. Restarts noise, jitter, histories and animation at the count's start, so captures repeat as far as the driver allows ([visual testing](visual-testing-plan.md)). |
| `SPEEDO_CAPTURE_PATH` | An existing folder. None. | Saves `pose-NNNN.bmp` for each `SPEEDO_CAMERA_PATH` pose. |
| `SPEEDO_CAPTURE_PATH_FIRST` | Pose index. 0. | Captures from that pose on. |
| `SPEEDO_CAPTURE_PATH_STRIDE` | At least 1. 1. | Captures every n-th pose. |

## The window and presentation

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_HEADLESS` | Exactly `1` on. Off. | No window, surface or swapchain; frames draw into an image of `SPEEDO_WINDOW_SIZE`. |
| `SPEEDO_WINDOW_SIZE` | `<width>x<height>` pixels. 1280x720 in window system units. | Framebuffer size in pixels on any display; headless, the image size. |
| `SPEEDO_BACKGROUND` | Exactly `1` on. Off. | Opens the window behind others without focus (Linux: unfocused only), and skips the test assets offer. |
| `SPEEDO_FULLSCREEN` | `exclusive`, `borderless`, `native` (macOS). `exclusive`. | How `Alt+Enter` goes fullscreen; doesn't start fullscreen. |
| `SPEEDO_PRESENT_MODE` | `fifo`, `fifo_relaxed`, `mailbox`, `immediate` (else fifo). `fifo`. | Present mode; `immediate` uncaps the frame rate. D3D12 supports `fifo`, `mailbox`, `immediate`. |
| `SPEEDO_SWAPCHAIN_IMAGE_COUNT` | Within the surface's limits and 8. 3. | Swapchain images. |
| `SPEEDO_PRESENTATION_TEST` | Steps separated by `;`: `fullscreen`, `fullscreen=exclusive\|borderless\|native`, `present=<mode>`, `images=<count>`, `size=<w>x<h>`, `monitor=<index>`. None. | Diagnostic. One step every `SPEEDO_PRESENTATION_STEP_FRAMES` frames; fails if no frame is drawn for 10 s (`assettest.ps1 -Presentation`). |
| `SPEEDO_PRESENTATION_STEP_FRAMES` | At least 1. 20. | Frames between presentation test steps. |

## The interface

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_UI` | `0` off. On. | `0` draws frames without the interface (autoloads, camera and captures still work). |
| `SPEEDO_SHOW_ABOUT` | Exactly `1` on. Off. | Opens About Speedo at startup. |
| `SPEEDO_SHOW_LIGHTS` | Exactly `1` on. Off. | Opens View > Lights... |
| `SPEEDO_SHOW_CAMERAS` | Exactly `1` on. Off. | Opens View > Cameras... |
| `SPEEDO_SHOW_SKY` | Exactly `1` on. Off. | Opens the Physical Sky... window (without switching to the sky). |
| `SPEEDO_SHOW_GLOBAL_ILLUMINATION` | Exactly `1` on. Off. | Opens View > Rendering Features > Global Illumination... |
| `SPEEDO_SHOW_RAY_TRACING` | Exactly `1` on. Off. | Opens View > Rendering Features > Ray Tracing... |
| `SPEEDO_SHOW_STATISTICS` | Exactly `1` on. Off. | Opens View > Statistics... (debug and profile builds). |
| `SPEEDO_OPEN_MENUS` | Menu labels separated by `/`, parents too (e.g. `View/Presentation`). None. | Holds those menus open, for screenshots. |

## Time and animation

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_TIME_FRAME_SECONDS` | Seconds per frame. Real time. | Counts each frame as this long so runs match frame for frame (time of day, motion blur, indirect lighting blending, which takes it clamped to 1/480 to 1/4 s). |
| `SPEEDO_ANIMATION` | Index, or `none` for the rest pose. 0. | Which animation plays. |
| `SPEEDO_ANIMATION_TIME` | Seconds. Plays from 0. | Pauses animations at that time; Copy Scene writes it. |
| `SPEEDO_ANIMATION_FRAME_SECONDS` | Seconds per frame. Real time. | Advances animations a fixed step per frame; prints where it got to at exit. |

## The camera and lens

All are part of a pose. Glare, focus, motion blur and grain need a lens: the pinhole has none.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_LENS` | `pinhole`, `gauss-50`, `wide-28`, `portrait-85`, `vintage-50`, `anamorphic-50`, `anamorphic-40`, `anamorphic-75`, `anamorphic-50-133`, `wide-20`, `ultrawide-16`, `ultrawide-12`, `fast-35`, `tele-135`. `pinhole`. | Scene. The lens: sets focal length and apertures, draws vignetting, aberration and distortion. |
| `SPEEDO_SENSOR` | `full-frame`, `aps-c`, `super35`, `1-inch`, `phone`. `full-frame`. | Scene. Sensor size; the focal length is kept. |
| `SPEEDO_FOCAL_LENGTH` | mm, 1 to 5000. 75 degrees vertical field. | Scene. Field of view as a focal length; a lens sets its own. |
| `SPEEDO_LENS_GLARE` | `0` off. On. | Scene. Bloom, ghosts, starbursts, anamorphic streaks. |
| `SPEEDO_FOCUS` | `spot` (`auto`), `zone`, `wide`, `manual` (2 m), or meters. `spot`. | Scene. What the depth of field focuses on. |
| `SPEEDO_FOCUS_CONTINUOUS` | `0` off. On. | Scene. Autofocus measures every frame; off keeps the first focus until Focus Now. |
| `SPEEDO_MOTION_BLUR` | `0` off. On. | Scene. Smears what moves over the shutter. |
| `SPEEDO_GRAIN` | `0` off. On. | Scene. Sensor noise, stronger at high ISO. |

## Exposure and tonemapping

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_EXPOSURE_MODE` | `program` (`auto`), `aperture`, `shutter`, `manual`. `program`. | Scene. How the camera exposes; wins over `SPEEDO_AUTO_EXPOSURE`. |
| `SPEEDO_AUTO_EXPOSURE` | `0` manual. Program. | Scene. Off takes exposure from aperture, shutter and ISO (f/16, 1/160 s, ISO 100). |
| `SPEEDO_APERTURE` | f-number 0.7 to 64, within the lens's range. 16. | Scene. Aperture in manual and aperture priority; with a lens, depth of field. |
| `SPEEDO_SHUTTER` | Seconds or `1/<n>`, 1/8000 to 30. 1/160. | Scene. Shutter in manual and shutter priority; with a lens, motion blur length. |
| `SPEEDO_ISO` | 50 to 409600. 100. | Scene. ISO in manual; with a lens, grain. |
| `SPEEDO_EXPOSURE` | EV stops. 0. | Scene. Exposure compensation. |
| `SPEEDO_TONEMAPPER` | `pbr-neutral`, `aces`, `agx`, `reinhard`, `linear`. `pbr-neutral`. | Scene. The tonemapping curve. |
| `SPEEDO_UPSCALING` | `off`, `native`, `quality`, `balanced`, `performance`. `balanced`. | Scene. Temporal upscaling: native is antialiasing alone; the others render at 77%, 67%, 50% each way. |
| `SPEEDO_EXPOSURE_LOG` | Exactly `1` on. Off. | Diagnostic. Prints auto exposure metering (every frame for the first 120, then every 30th). |

## The environment, sky and time

All are part of a pose. Apart from intensity and rotation they are the physical sky's, which lights the scene unless a panorama is loaded or the model's directional light replaces it.

A model that comes with its reference's conditions (the glTF research set's `conversion.yaml` beside it) is opened in them, under any of these variables that are set: `SPEEDO_AUTOLOAD_ENVIRONMENT`, `SPEEDO_ENVIRONMENT_INTENSITY`, `SPEEDO_AUTO_EXPOSURE=0` with `SPEEDO_APERTURE=16`, `SPEEDO_SHUTTER=1/160`, `SPEEDO_ISO=100` and `SPEEDO_EXPOSURE`, `SPEEDO_TONEMAPPER=pbr-neutral`, `SPEEDO_GROUND_PLANE=0` and `SPEEDO_SOLID_TRANSMISSION=1`; the client prints them as it reads them.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_ENVIRONMENT_INTENSITY` | Multiplier, 0 or more. 1. | Scene. Scales sky or panorama, sun or moon, and air light. |
| `SPEEDO_SKY_WITH_MODEL_LIGHTS` | Not `0` on. Off. | Scene. Keeps the physical sky where the model has its own directional light (which otherwise replaces it). |
| `SPEEDO_ENVIRONMENT_ROTATION` | Degrees. 0. | Scene. Turns the environment and its sun about the vertical axis. |
| `SPEEDO_TIME_OF_DAY` | Hours (`18.5`) or `h:m` (`18:30`), 0 to 24. 10.5. | Scene. Local solar time: sun, moon, stars. |
| `SPEEDO_TIME_SPEED` | Times real time, negative backwards. Stands still. | Scene. Advances the time of day (60 is an hour a minute). |
| `SPEEDO_LATITUDE` | Degrees, -90 to 90. 48.86. | Scene. Where on Earth the sky is seen from. |
| `SPEEDO_DAY_OF_YEAR` | 1 to 365. 172. | Scene. The date: sun height and moon place. |
| `SPEEDO_YEAR` | 1900 to 2200. Current year. | Scene. The year for the moon's place and phase. |
| `SPEEDO_VISIBILITY` | km, 1 to 288.5. 217.4. | Scene. Air clarity: sky, sun light, haze, light shafts. Not the visibility buffer. |
| `SPEEDO_AERIAL_PERSPECTIVE` | `0` off. On. | Scene. Distant haze; fog and light shafts need it. |
| `SPEEDO_LIGHT_SHAFTS` | `0` off. On. | Scene. Sun (at night moon) shadows in the air. Needs aerial perspective and shadows. |
| `SPEEDO_FOG` | `0` off. On. | Scene. Ground haze. Needs aerial perspective. |
| `SPEEDO_FOG_VISIBILITY` | km, 0.05 to 1000. 30. | Scene. Visibility at the ground: 30 clear, 2 mist, 0.2 fog. |
| `SPEEDO_FOG_HEIGHT` | m, 1 to 5000. 200. | Scene. Height at which fog is about a third as dense as at the ground. |

## Lighting, shadows and ray tracing

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_SHADOWS` | `0` off. On. | Scene. Shadow atlas shadows; off also stops traced shadows. |
| `SPEEDO_TRACED_SHADOWS` | `0` off, `all` at every pixel. On, where the atlas is unsure. | Scene. Traces the brightest lights' shadows (up to 4) through the BVHs. |
| `SPEEDO_TRACED_REFLECTIONS` | `0` off. On. | Scene. Traces reflections of smooth and glossy surfaces and clear coats. |
| `SPEEDO_REFLECTION_STRIDE` | `1`, `2`, `4`. Device's: 1 with ray queries, 4 through BVHs. | Scene (written where set). One reflection ray per n-th pixel each way. |
| `SPEEDO_FRAGMENT_REFLECTIONS` | `0` or `1`. Device's: on with ray queries. | Scene (written where set). Traces blended and transmissive surfaces' sharp reflections. |
| `SPEEDO_TRACED_REFRACTIONS` | `0` off. Device's: on with ray queries, off through BVHs. | Scene (written where set). Traces what shows behind transmissive surfaces. |
| `SPEEDO_GLASS_SURFACE_COUNT` | 1 to 8. Device's: 8 with ray queries, 4 through BVHs. | Scene (written where set). Most transmissive surfaces a refraction or caustic photon crosses. |
| `SPEEDO_SOLID_TRANSMISSION` | Not `0` on. Off (on for research set models). | Scene. Transmissive materials without `KHR_materials_volume` refract as solids, not thin walls. |
| `SPEEDO_CAUSTICS` | `0` off. Device's: on with ray queries. | Scene. Caustics from lights through still transmissive surfaces, and from emitters through solid glass. |
| `SPEEDO_EMISSIVE_LIGHTS` | `0` off. On. | Scene. Emissive surfaces as area lights; off, only indirect lighting sees them. |
| `SPEEDO_LIGHT_SIZES` | `<light>=<size>` pairs, comma separated: light index (`#n` in View > Lights...), degrees for directional, scene units for point and spot. Sun's size, else 0. | Softens the model's lights' shadows once a model is in. |
| `SPEEDO_MERGE_LIGHTS` | `0` off. On. | Merges the environment's sun with a model's directional light within 5 degrees (the model's). |
| `SPEEDO_CLUSTERED_LIGHTS` | `0` off. On. | Each pixel loops over its cluster's lights instead of all. |
| `SPEEDO_SHADOW_CACHE` | `0` off. On. | Keeps shadow atlas tiles while their views don't change. |

On macOS the rays are traced in software by default (`SPEEDO_RAY_QUERY=1` or View > Rendering Features > Ray Tracing... > Ray Queries for hardware, see [the gpu and drivers](#the-gpu-and-drivers)); every traced feature runs either way, on smaller budgets in software.

## Indirect lighting

Variables marked Scene are View settings; the rest is the renderer's tuning ([indirect lighting](notes/indirect-lighting.md)).

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_GI` | `0` off. On. | Scene (always written). Indirect lighting: irradiance probes over a voxel clipmap. |
| `SPEEDO_GI_RESOLUTION` | Voxels on the longest axis, 8 to 512. 128. | Scene. Last clip's resolution; finer clips follow. Finer costs steeply. |
| `SPEEDO_GI_TRACE` | `voxels`, else triangles. Triangles. | Scene. What rays meet: triangles via BVHs (where present) or voxels. Near field needs triangles. |
| `SPEEDO_GI_CLIP_COUNT` | 1 to 4. 3. | Clipmap levels: the last covers the scene, each finer has half-size voxels and follows the eye. |
| `SPEEDO_GI_LOOK_AHEAD` | 0 to 1 of a clip's half size. 0.75. | How far ahead of the eye finer clips are centered. |
| `SPEEDO_GI_PROBE_SPACING` | Voxels: 8, 16, 24, 32 (else startup fails). 8. | Probe spacing. |
| `SPEEDO_GI_EMISSION_REACH` | Probe spacings, 0 to 8 (0 no limit). 0.5. | Treats emissive hits nearer than this as from this far. Only with emissive lights off. |
| `SPEEDO_GI_BOUNCE` | `0` off. On. | Voxels reflect the probes' last light (a bounce a frame); off, direct light only. |
| `SPEEDO_GI_RAY_COUNT` | 8 to 1024. 128. | Most rays a probe traces per update. |
| `SPEEDO_GI_RAY_BUDGET` | Rays per frame, at least 1. 500000. | Shared among traced probes, lowering their rays (to 16 at least). |
| `SPEEDO_GI_RAY_PHASE_COUNT` | 1 to 256. 32. | Updates before ray directions repeat. |
| `SPEEDO_GI_HYSTERESIS` | 0 to 0.99. 0.97. | Share of its light a probe keeps per frame (at 60 fps if time based). |
| `SPEEDO_GI_TIME_BASED` | `0` off. On. | Blends by elapsed time, not per frame, and lets the surface cache window grow and settle. |
| `SPEEDO_GI_ADAPT` | Exactly `1` on. Off. | Probes blend faster through large light changes. |
| `SPEEDO_GI_PROBE_FREEZE` | `0` off. On. | After the scene is still a while, settled probes keep their light, every second cycle. |
| `SPEEDO_GI_SIDES_KEEP_FRAME_COUNT` | 0 to 1024. 16. | Every how many frames a pixel's lookups' masks are traced again, a tile of pixels at a time; between, it keeps the last frame's. 0 traces every mask every frame. |
| `SPEEDO_GI_STAGGER` | `0` off, `coarser`. On. | The clip levels take turns, one a frame, once every clip's surface cache is warm; until then, and with `coarser`, the finest clips update every frame and the coarser ones take turns; off updates all every frame. |
| `SPEEDO_GI_RELIGHT_PHASE_COUNT` | 1 to 64. 4. | Updates over which static voxels are relit when lights change gradually. |
| `SPEEDO_GI_CACHE` | `0` off. On. | Scene. Surface cache: voxel rays accumulated and filtered along surfaces. |
| `SPEEDO_GI_CACHE_RAYS` | 1 to 64 per voxel. 1. | The cache's rays per update. |
| `SPEEDO_GI_CACHE_WARM_RAYS` | 1 to 64 per voxel. 4. | Scene. Rays per update during warm-up (first 256 updates after a restart); the menu offers 4, 8, 16. More settles smoother but costs while it lasts. |
| `SPEEDO_GI_CACHE_SAMPLES` | 1 to 65535. 4096. | Most samples a voxel's average covers. |
| `SPEEDO_GI_CACHE_FAST_SAMPLES` | 0 to 1024 (0 off). 16. | Samples of a voxel's fast average, which sets how soon each bounce follows the last. |
| `SPEEDO_GI_CACHE_SEEN_SECONDS` | 0 to 60. 1. | How long after last being read a block is still filtered (at least 2 updates). |
| `SPEEDO_GI_CACHE_FILTER_PASSES` | 1 to 4. 3. | Filter passes along surfaces. |
| `SPEEDO_GI_CACHE_SIGMA` | 0 or more. 8. | Noise deviations a neighbour may differ by and still be averaged in. |
| `SPEEDO_GI_CACHE_SETTLED_FRAMES` | Updates. 2048. | After this many updates the cache is settled and voxels trace every other frame. |
| `SPEEDO_GI_WORLD` | `1` on. Off. | Scene. World cache ([plan](world-cache-plan.md)), Light Store in the menu: world-anchored multi-resolution cells filled by the surface cache's rays; the clipmap's light where cells don't cover. Shown by the `gi-world` debug view. |
| `SPEEDO_GI_WORLD_CELLS` | Power of two, 1024 to 16777216. 1048576. | World cache cells (148 bytes each, plus sums slots for a quarter, 112 bytes); least recently used are dropped when full. |
| `SPEEDO_GI_WORLD_LEVELS` | 1 to 8. 5. | World cache levels, each cell twice the side of the one below. |
| `SPEEDO_GI_CACHE_KEEP_FRAMES` | Frames. 1024. | How long a voxel block keeps its cache page after last read. Not for triangle tracing, where surface blocks always keep theirs. |
| `SPEEDO_GI_CACHE_EVERYWHERE` | Exactly `1` on. Off. | Traces and filters every voxel near a surface: for comparisons. |
| `SPEEDO_GI_NEAR` | `0` off. On. | Scene. Near field: short pixel rays light surfaces near others, darkening contacts smoothly. Needs triangles. |
| `SPEEDO_GI_NEAR_REACH` | Voxels, 0 to 64 (0 off). 8. | Near field ray length. |
| `SPEEDO_GI_NEAR_CONTACT` | Voxels, 0 to 8, within reach. 2.5. | Scene. How near a hit counts as contact, where ray light replaces voxel light. |
| `SPEEDO_GI_NEAR_POWER` | 0 to 64. 2. | Weight of voxels against rays: the share of rays without contact to this power. |
| `SPEEDO_GI_NEAR_STRIDE` | `2`, else 4. 4. | Scene. Near field from every 2nd or 4th pixel each way. |
| `SPEEDO_GI_NEAR_TEMPORAL` | `0` off. On. | Scene. Accumulates near field rays over frames, reprojected. |
| `SPEEDO_GI_NEAR_HISTORY_FRAME_COUNT` | 1 to 1024. 16. | Frames the near field history holds. |
| `SPEEDO_GI_NEAR_STILL_FRAME_COUNT` | 1 to 2048. 256. | Longer history while view, model and lights are still. |
| `SPEEDO_GI_GATHER` | `1` on. Off. | Scene. Final gather: opaque surfaces' indirect light gathered by rays from each pixel instead of looked up in the voxels. Needs triangles. |
| `SPEEDO_GI_GATHER_STRIDE` | `1`, `2`, `4`. 2. | Scene. Final gather from every pixel, or every 2nd or 4th each way. |
| `SPEEDO_GI_DEBUG` | 1 to 15. None. | Diagnostic. Starts in that indirect lighting debug view; `SPEEDO_DEBUG_VIEW` wins. |
| `SPEEDO_GI_REFERENCE_RAYS` | 16 to 4096 per pixel. 256. | Diagnostic. Rays of the Surface Reference view; lower if a frame times the gpu out. |
| `SPEEDO_GI_CACHE_SHOW` | Exactly `1` on. Off. | Diagnostic. Voxels show the surface cache's light alone. |
| `SPEEDO_GI_LOOKUP_LEVEL` | 0 (finest) to 3. Off. | Diagnostic. Shading ignores levels finer than this. |
| `SPEEDO_GI_HIT_SKIP` | `far-side`, `apart`, `beside`, `coarser`, joined by `,` or `+`. None. | Diagnostic. The tests of a hit's voxel choice the probes' and the surface cache's rays do without, compiled out of their kernels. Needs specialization constants. |
| `SPEEDO_GI_NEAR_HIT_SKIP` | As `SPEEDO_GI_HIT_SKIP`. None. | Diagnostic. The same for the near field's and the final gather's rays. |
| `SPEEDO_REFLECTION_HIT_SKIP` | As `SPEEDO_GI_HIT_SKIP`. None. | Diagnostic. The same for the Reflection Trace pass's hits. |
| `SPEEDO_FRAGMENT_HIT_SKIP` | As `SPEEDO_GI_HIT_SKIP`. None. | Diagnostic. The same for the fragments' traced rays' hits (sharp reflections, refractions), where the materials' pipelines are specialized. |
| `SPEEDO_GI_CACHE_THIN` | `0` off, `1` single sided boards, `2` double sided sheets too. `2`. | To compare. Lookups beside something thinner than a voxel skip voxels of its other side. |
| `SPEEDO_GI_PROBES` | `0` none, `1` where they show, `2` every voxel. `1`. | To compare. Which voxels get probe light each frame: `1` only those it can change, `2` all near surfaces, `0` none (probes don't trace). Without the surface cache, or when rays walk voxels, all voxels regardless. |
| `SPEEDO_GI_PROBE_AIR_INTERVAL` | 1 to 255. 8. | Updates between resolving probe light into air-only voxels, while lights are still. |
| `SPEEDO_GI_NEAR_SHOW` | 0 to 4. 0. | Diagnostic. Near field: 1 ray light, 2 share without contact, 3 light of contact rays, 4 of the others. With the final gather: 1 the rays' light, 2 the share of rays whose hits are known. |
| `SPEEDO_GI_DUMP` | `x,y,z` or `pixel:x,y`. Off. | Diagnostic. At exit prints each clip's probe nearest the point; clips then update every frame. |
| `SPEEDO_GI_EYE_PATH` | Points `x,y,z[,lx,ly,lz]` separated by `;` or new lines, or `@<file>`. The camera. | Diagnostic. Centers clips on these points, one a frame, then the last. |
| `SPEEDO_GI_EYE_LOG` | Exactly `1` on. Off. | Diagnostic. Prints each frame's clip center and look, and clip scrolls. |
| `SPEEDO_GI_REVOXELIZE` | Exactly `1` on. Off. | Diagnostic. Revoxelizes every clip each frame, restarting probes. Slow. |

## Emissive surfaces

Tuning of the emissive surfaces' light (`SPEEDO_EMISSIVE_LIGHTS`), for a model with emitters and BVHs.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_EMITTER_STRIDE` | 1, 2, 4, 8. 4. | Gathers light at every n-th pixel each way. |
| `SPEEDO_EMITTER_CANDIDATE_COUNT` | 1 to 256. 8. | Candidate points a traced pixel resamples before its shadow ray. |
| `SPEEDO_EMITTER_VOXEL_SAMPLE_COUNT` | 1 to 256. 4. | Samples (each a shadow ray) averaged per indirect lighting voxel. |
| `SPEEDO_EMITTER_VOXEL_CANDIDATE_COUNT` | 1 to 256. 8. | Candidates resampled per voxel sample. |
| `SPEEDO_EMITTER_REUSE` | Exactly `1` on. Off. | Reuses reservoirs from last frame and neighbours. |
| `SPEEDO_EMITTER_HISTORY_CAP` | 1 to 1000. 20. | How much last frame's reservoir may count, with reuse. |
| `SPEEDO_EMITTER_SPATIAL_COUNT` | 0 to 8. 0. | Neighbours a pixel merges reservoirs with, with reuse. |
| `SPEEDO_EMITTER_FILTER_REACH` | Pixels, 1 to 64. 16. | Reach of the light's row and column filter. |
| `SPEEDO_EMITTER_HISTORY_FRAME_COUNT` | 1 to 1024. 32. | Frames the light is averaged over. |
| `SPEEDO_EMITTER_STILL_FRAME_COUNT` | 1 to 2048. 256. | Longer average while camera and model are still. |
| `SPEEDO_EMITTER_VISIBILITY_FRAME_COUNT` | 1 to 1024. 8 (a quarter for moving models). | While the camera is still, frames a visible point is trusted before retracing. |
| `SPEEDO_EMITTER_NEW_SAMPLE_COUNT` | 1 to 64. 16. | Samples a newly visible pixel's light is the mean of. |
| `SPEEDO_EMITTER_NEW_CANDIDATE_COUNT` | 1 to 256. 8. | Emitters each of those samples picks from (fewer is cheaper and noisier). |
| `SPEEDO_EMITTER_ROULETTE` | 0 or more (on screen brightness). 0.02. | Below it shadow rays trace only by chance; 0 traces all. |
| `SPEEDO_EMITTER_CAUSTIC_PHOTON_COUNT` | 0 to 4194240. 262144. | Photons per frame from emitters through solid glass, when caustics are on (`SPEEDO_CAUSTICS`); 0 none. |
| `SPEEDO_EMITTER_CAUSTIC_FRAME_COUNT` | 1 to 1048576. 4096. | Most frames the emitter caustics are averaged over while still: longer is smoother, slower to follow changes. |

## Culling and drawing

Renderer settings, except `SPEEDO_CULLING`, which sets View > Rendering Features > Culling's starting state.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_CULLING` | `0` off. On. | Drops draws outside each view's frustum; off also disables occlusion culling. |
| `SPEEDO_INDIRECT_DRAWS` | `0` off. On. | One indirect draw per run of same-state draws; off uses direct draws without gpu or occlusion culling. |
| `SPEEDO_GPU_CULLING` | `0` off. On. | The gpu culls draws; off leaves frustum culling to the cpu, without occlusion culling. |
| `SPEEDO_OCCLUSION_CULLING` | `0` off. On. | Drops main view draws hidden behind others, using a depth pyramid. Needs gpu culling. |
| `SPEEDO_OCCLUSION_MODE` | `two-phase`, `reprojected`, `hybrid`. `two-phase`. | How hidden draws are found: last frame's visible then the rest, last depth reprojected, or both. |
| `SPEEDO_SHADOW_OCCLUSION_CULLING` | Exactly `1` on. Off. | Also occlusion culls shadow views. |
| `SPEEDO_VISIBILITY_BUFFER` | `0` off. On. | Shades opaque surfaces from the pre-pass's visibility buffer. |
| `SPEEDO_SHADER_TIERS` | `0` off. On. | Pipelines that skip extended layers (clearcoat, sheen, transmission, ...) for plain materials; off uses generic ones. |
| `SPEEDO_SHADER_STATE_TIERS` | `0` off. On where the api has specialization constants (not D3D12). | Pipelines without what the frame leaves unused; off uses generic ones. `SPEEDO_SHADER_TIERS=0` turns every tier off. |
| `SPEEDO_BVH_BUILD` | `fast`, else spatial splits. Spatial splits. | Static BVH build: tinybvh's SBVH (fewer nodes visited) or binned SAH. Each is cached apart. |

## Diagnostics

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_DEBUG_VIEW` | A debug view's name or number (below). The lit image. | Scene. Shows surface values, parts of the light, or indirect lighting internals instead of the lit image. |
| `SPEEDO_GPU_TIMINGS` | Exactly `1` on. Off. | At exit prints median frame time, each gpu pass's median, occlusion culling draws and indirect lighting statistics. On KosmicKrisp use the draw thread's frame time: its timestamps miss compute passes. |
| `SPEEDO_RUN_TIMINGS` | Exactly `1` on. Off. | At exit prints the run's milestones (process, device, layouts, pipelines, autoloads, camera path, exit count, capture, shutdown) with seconds since start, step time and frames drawn. |
| `SPEEDO_SKIP_PASSES` | Exact frame graph pass names separated by `;` (e.g. `GI Near;Emitter Shade`; within GI also `GI Light`, `GI Flags`, `GI Trace`, `GI Probe Update`, `GI Cache Release`, `GI Resolve`, `GI Cache Filter`). None. | Leaves those passes out to measure their cost; the image is wrong. |
| `SPEEDO_LOG_PIPELINES` | Exactly `1` on. Off. | Logs every pipeline made and its time; otherwise only those taking a second or more. |
| `SPEEDO_TRACY` | `0` off. On (profile builds). | Whether Tracy records zones, frames and gpu zones. It buffers every event until a profiler connects, so idle clients grow fast; the test scripts set it to `0`. |
| `SPEEDO_SHADER_LOG` | Exactly `1` on. Off. | Prints shader compiler details: paths, reflection, bindings, includes. |
| `SPEEDO_SHADER_TIMINGS` | Exactly `1` on. Off. | Diagnostic. Prints each shader compile's rounds or phases and each scene layout's load time ([plan](shader-compile-plan.md)). |
| `SPEEDO_SHADER_WORKER_COUNT` | 0 to 64. Half the threads, at most 8. | Worker processes (`shadercompileworker`) a shader compile runs in; code is cached in `shader-cache` beside the executable. `0` compiles in the client's process without that cache. |
| `SPEEDO_SHADER_RELOAD` | `0` off. On (in a checkout). | Reloads shaders when sources change, keeping state; a failed compile keeps the old shaders, and changed bindings or push constants need a restart. Installs have no sources. |
| `SPEEDO_CRASH_BREADCRUMBS` | `markers`, `checkpoints`, `none`. Markers if available, else checkpoints. | Source of the crash reports' in-flight passes (`VK_AMD_buffer_marker` or `VK_NV_device_diagnostic_checkpoints`). |
| `SPEEDO_CRASH_TEST` | `hang`. Off. | From frame 100 waits on a semaphore nothing signals, to test hang reports (with a short `SPEEDO_GPU_HANG_TIMEOUT`). |
| `SPEEDO_RAY_QUERY_WATCH` | Not `0` on. Off. | Gives each dispatch a breadcrumb and records ray queries in flight for hang reports. |
| `SPEEDO_RAY_QUERY_REPLAY` | Text file of rays, one per line: origin, direction, tmax, any hit (0 or 1), moving (0 or 1). Off. | Traces those rays every frame after indirect lighting to find one that hangs the gpu. Turns the watch on. |
| `SPEEDO_RAY_QUERY_REPLAY_REPEAT` | Count. 64. | Times per frame each replayed ray is traced. |
| `SPEEDO_VALIDATE_SYNC` | Set (any value, even `0`) on. Off. | Validation layer synchronization checks (slow); used in debug builds and the scripts' `-Validate`. |
| `SPEEDO_VALIDATION_BREAK` | `error`, `warning`, `off`. Off. | Breaks into an attached debugger on validation messages of that severity or worse (debug builds). |

The debug views, by name (and number): `base-color` (1), `normal` (2), `geometry-normal` (3), `roughness` (4), `metallic` (5), `occlusion` (6), `material` (7), `texcoord` (8), `shadows` (9), `light-count` (10), `emissive` (11), `direct` (12), `indirect` (13), `indirect-diffuse` (14), `indirect-specular` (15), `gi-irradiance` (16), `gi-visibility` (17), `gi-voxel-behind` (18), `gi-voxel-front` (19), `gi-voxel-albedo` (20), `gi-reference` (21), `gi-clip` (22), `gi-grid` (23), `gi-voxel-world` (24), `gi-voxel-world-albedo` (25), `gi-voxel-world-normal` (26), `gi-probes` (27), `gi-surface-reference` (28), `gi-probe-state` (29), `gi-world` (30), `traced-shadow` (31), `traced-shadow-raw` (32), `traced-shadow-reference` (33), `emitters` (34), `emitters-raw` (35), `emitters-reference` (36), `traced-reflections` (37). The `gi-*` views need indirect lighting; the reference views take seconds a frame. An unknown name isn't understood, as for any pose variable.

## The gpu and drivers

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_GRAPHICS_API` | `vulkan`, `metal` (macOS), `d3d12` (Windows). `vulkan` on Windows and Linux, `metal` on macOS. | The graphics api to run on ([build](notes/build.md)); `metal` and `d3d12` need no `VK_DRIVER_FILES`. An unknown name or another platform's api fails at start. |
| `SPEEDO_RAY_QUERY` | `1` hardware at start (macOS); `0` none. Hardware where the device has it, software on macOS. | How rays are traced at startup: hardware ray queries or software (shaders traverse BVHs); switchable in Ray Tracing... > Ray Queries. `0` leaves hardware off. Software is macOS's default because Metal's ray tracing restarts the gpu now and then ([indirect lighting](notes/indirect-lighting.md)). |
| `SPEEDO_RAY_QUERY_SWITCH_FRAME` | Frame count. None. | Diagnostic. Switches ray query mode once, that many frames after the scene is ready. |
| `SPEEDO_EXECUTOR_THREAD_COUNT` | At least 1. Hardware threads less 2. | Task executor workers; `2` reproduces a small machine. |
| `SPEEDO_GPU_HANG_TIMEOUT` | Seconds; 0 or text never. 20. | How long a gpu wait or the draw thread without heartbeat may last before a hang report (in the profile's `crashes`) and exit. |
| `SPEEDO_D3D12_VALIDATION` | Exactly `1` on. Off (on in debug builds). | D3D12 debug layer; errors print as `D3D12 Validation Error`. Needs Windows' Graphics Tools (`SPEEDO_D3D12_DEBUG_LAYER`). |
| `SPEEDO_D3D12_GPU_VALIDATION` | Exactly `1` on. Off. | With `SPEEDO_D3D12_VALIDATION`, gpu based validation (much slower). |
| `SPEEDO_D3D12_WARP` | `1` Windows' WARP, or a `d3d10warp.dll` path (`scripts/fetch-warp.ps1` prints it). Off. | D3D12 on WARP, the software adapter, for machines without a gpu. A path loads the NuGet WARP (testing only), which runs ray queries that crash Windows' ([backends plan](backends-plan.md)). |
| `SPEEDO_PIPELINE_BINARIES` | `0` off. On where the device has `VK_KHR_pipeline_binary`. | Vulkan: per-pipeline binary files (the profile's `pipeline.binaries/`, least recently used dropped past 512 MB) instead of a pipeline cache file per version of the shaders ([rhi](notes/rhi.md), "Pipeline caches"). |
| `SPEEDO_D3D12_DRED` | `0` off. On. | D3D12 device removed reports (DRED breadcrumbs, contexts, page faults) for crash reports. |

## The scripts and the build

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_BINARY_CACHE` | `0` off. On. | Setup and vcpkg read the shared binary cache, and FASTBuild's cache plugin its remote; `0` keeps both local ([build](notes/build.md)). |
| `SPEEDO_BINARY_CACHE_WRITE` | Any non-empty value on. Off. | Also writes this machine's builds to the shared caches, with AWS credentials from the environment (`AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`, or `AWS_PROFILE`); setup installs the AWS CLI. |
| `SPEEDO_FASTBUILD_CACHE` | `0` off. On. | The presets' FASTBuild launcher runs FBuild with its cache; `0` runs it as asked. |
| `FASTBUILD_CACHE_MODE` | `r`, `w`, `rw`. Unset. | Set, the launcher leaves the cache mode to FBuild instead of adding `-cache`. |
| `ASSETTEST_CLIENT_TIMEOUT` | Seconds. 600. | How long `assettest.ps1` gives each client run. |
| `SPEEDO_CLIENT_KILL_WAIT_SECONDS` | Seconds. 30. | How long the test scripts wait for a timed out client to end once killed; one that hasn't stops the run as a wedged gpu (`scripts/gpu.ps1`). `0` simulates that, to test it. |
| `SPEEDO_D3D12_DEBUG_LAYER` | Exactly `1` on. Off. | Setup on Windows installs D3D12's debug layer (Graphics Tools) if missing, asking once for administrator rights; if declined the D3D12 backend runs unvalidated. |
| `SPEEDO_VCPKG_ROOT` | A folder. `~/.vcpkg-tool/<release>`. | Setup uses the vcpkg deployment there instead of downloading the release `scripts/vcpkg-tool.json` pins. |
| `SPEEDO_PROFILER_APP` | `0` leaves it out. In. | Setup and the presets' configures leave out Tracy's profiler app (the manifest's `profiler-app` feature); CI and the release workflow set `0`. |

`SPEEDO_TEST_ASSETS` and `SPEEDO_VALIDATE_SYNC` above are the scripts' too. Variables like `SPEEDO_TARGET_CPU` and `SPEEDO_SANITIZE` are CMake's, set by the presets, not the environment's.

## The Vulkan loader

| Variable | What it does |
|---|---|
| `VK_DRIVER_FILES` | The driver's manifest: KosmicKrisp's on macOS from a checkout, or lavapipe's to render without a gpu (see the [README](../README.md#3-run-the-client)). |
| `VK_LAYER_PATH` | The validation layer's manifest directory for a debug build from a shell: `install/<triplet>/share/vulkan/explicit_layer.d` (`bin` on Windows). The scripts' `-Validate` set it. |
| `VK_LOADER_DEBUG` | The loader's logging (`all`, `error`, ...). A debug build prints it, `VK_LAYER_PATH` and `VK_DRIVER_FILES` at startup. |
| `MESA_KK_EXPERIMENTAL` | KosmicKrisp's experimental features; the client sets `ray_query` where ray queries are on (macOS). |
| `__GL_SHADER_DISK_CACHE_PATH`, `__GL_SHADER_DISK_CACHE_SIZE` | NVIDIA's compiled pipeline cache location and size in bytes. The client sets them unless set: `driver-cache` beside the executable in a checkout, the profile's in an install, 10 GB ([layering](notes/layering.md)). |
