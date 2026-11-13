# Environment variables

The client reads these at startup. They start it in a known state for bug reports, measurements, screenshots and tests: what to load, where the camera is, every View setting, a capture and an exit after so many frames, a window of a given size or no window at all, and the renderer's tuning and diagnostics. The scripts and the build read a few of their own, listed at the end.

View > Camera > Copy Pose (`Ctrl/Cmd+Shift+C`) writes the current view as these variables, and Paste Pose (`Ctrl/Cmd+Shift+V`) applies them to a running client: that is the easiest way to get a set of them. For example:

```sh
SPEEDO_AUTOLOAD_MODEL=test-assets/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf SPEEDO_CAMERA=6.14,4.28,-2.41,-18.4,-104.6 SPEEDO_TIME_OF_DAY=14.5 SPEEDO_HEADLESS=1 SPEEDO_UI=0 SPEEDO_WINDOW_SIZE=1280x720 SPEEDO_AUTOLOAD_EXIT=200 SPEEDO_CAPTURE=sponza.bmp ./build/arm64-osx-clang-profile/client
```

This loads Sponza, places the camera in its gallery at half past two, renders headless without the interface, and saves the 200th frame and exits. On macOS from a checkout, set `VK_DRIVER_FILES` to KosmicKrisp first (see the [README](../README.md#3-run-the-client)).

## How they are read

- **Pose** marks a variable that is part of a pose: Copy Pose writes it (when it differs from the default), and Paste Pose applies it, resetting each pose variable the pasted text leaves out to its default. The View menu can change these while the client runs.
- Pose variables follow common rules: an empty value counts as unset; names are exact and case-sensitive; a value that isn't understood prints `NAME=value: not understood` and leaves the setting alone; for a switch, `0` turns it off and any other value on; a number is clamped to the range given, and text that isn't a number reads as 0 before clamping.
- The other variables each parse their own way, as their rows say. Several turn on only with exactly `1`.
- The renderer's tuning (most of the indirect lighting's, the emissive surfaces', culling) is read once, when the renderer is created, and stays fixed for the run; none of it is in the View menu.
- Paths: the files to load (`SPEEDO_AUTOLOAD_MODEL`, `_ENVIRONMENT`, `_IMAGE`, `SPEEDO_BROWSE`) are absolute or relative to the resource directory, `resources/` in a checkout (run the client from the repository root) or `-r <dir>`. Output files and folders (`SPEEDO_CAPTURE`, `SPEEDO_CAPTURE_PATH`, `SPEEDO_CAMERA_PATH=@file`, `SPEEDO_TEST_ASSETS`) are relative to the working directory.
- Nothing set here is saved between runs, but for the unit `SPEEDO_UNITS` gives an autoloaded model, which the profile remembers: use a scratch profile (`-u <dir>`) for tests.

## Loading at startup

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_AUTOLOAD_MODEL` | A model file (`.gltf`, `.glb`, `.obj`), a folder or zip archive of models, or several files separated by `;` (quote it in a shell). Default: none. | Pose. Loads it at startup as File > Open... would. A folder's or archive's models load side by side; a model with material variants gets its default materials. |
| `SPEEDO_AUTOLOAD_SCENE` | A glTF scene's index, from 0. Default: the file's default scene, else its first. | Pose. Loads that scene of a single autoloaded glTF file, as View > Scene does. |
| `SPEEDO_AUTOLOAD_VARIANT` | A material variant's index, from 0. Default: the default materials. | Pose. Loads a single autoloaded model with that `KHR_materials_variants` variant's materials. |
| `SPEEDO_AUTOLOAD_VARIANTS` | `all`, or indices and `default` separated by commas (e.g. `default,0,2`); indices past the file's count are dropped. Default: none. | Pose. Loads a copy of the model per variant, one behind the other by default, as File > Open...'s Variants dialog does. |
| `SPEEDO_AUTOLOAD_ARRANGEMENT` | `side-by-side`, `as-authored` or `behind-each-other`. Default: side by side for a `;` list, behind each other for variants. | Pose. How several autoloaded files, or the variants' copies, are arranged. A single folder or archive is always side by side. |
| `SPEEDO_GROUND_PLANE` | `0` off. Default: on. | Pose. Whether the autoloaded model stands on a ground plane (gray squares a meter each), as File > Open...'s Ground plane box. |
| `SPEEDO_UNITS` | `m`, `cm`, `mm`, `in` or `ft` (or the words: `meters`, `inches`, `feet`, ...), any case. Default: the unit the profile remembers for the file, else a guess (glTF in meters, OBJ by its size). | Pose. The unit the model's coordinates are in, as View > Units; wins over `SPEEDO_METERS_PER_UNIT`. With `SPEEDO_AUTOLOAD_MODEL` the profile remembers it for that file. |
| `SPEEDO_METERS_PER_UNIT` | Meters per unit, at least 0.000001. Default: as `SPEEDO_UNITS`. | Pose. A unit of any size, as View > Units > Meters per Unit. |
| `SPEEDO_AUTOLOAD_ENVIRONMENT` | A panorama (`.hdr`). Default: the physical sky. | Pose. Lights the scene with the panorama, as View > Environment > Open Environment... does. |
| `SPEEDO_AUTOLOAD_IMAGE` | An image file. Default: none. | Loads the image onto the default material, as opening an image in File > Open... does. |

## The camera and views

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_CAMERA` | `x,y,z,pitch,yaw` (the eye in world space, in the scene's units; pitch and yaw in degrees), or `frame`. Default: the model framed, or its first camera. | Pose (Paste Pose needs it). Places every view once the autoloads are in; `frame` is View > Camera > Frame model. |
| `SPEEDO_LAYOUT` | `1x1`, `1x2`, `2x1` or `2x2` (columns x rows). Default: `1x1`. | Pose. The split screen's views, as View > Layout (`Ctrl/Cmd+1` to `4`). |
| `SPEEDO_CAMERA_VIEWS` | Poses as `SPEEDO_CAMERA`'s, separated by `;`, for views 1 to 3; or `around`. Default: `SPEEDO_CAMERA` places every view. | Pose. Places split screen's other views; the poses apply only with a pose in `SPEEDO_CAMERA`. `around` turns views 1 and on about the scene's center, evenly apart. |
| `SPEEDO_CAMERA_PATH` | Poses as `SPEEDO_CAMERA`'s, separated by `;` or new lines, or `@<file>` holding them. Default: none. | Puts the views at one pose per drawn frame, after `SPEEDO_CAMERA`'s placement, then holds the last: a repeatable move. The autoload exit waits for it. |
| `SPEEDO_CAMERA_TURN` | Degrees per second; negative turns the other way. Default: 0. | Turns every view about the vertical axis without stopping, once a model is in: a moving camera for comparing renders and timings. |
| `SPEEDO_CAMERA_WALK` | Scene units per second along the view; negative goes back. Default: 0. | Moves every view forward without stopping, as `SPEEDO_CAMERA_TURN` starts; the two combine. |
| `SPEEDO_PASTE_POSE_FRAME` | A drawn frame's count. Default: never. | At that frame, pastes the clipboard's pose once, as Paste Pose: for testing it. |

## Captures and exiting

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_AUTOLOAD_EXIT` | A count of drawn frames. Default: the client keeps running. | Exits that many frames after the loads are installed, the pipelines made, the camera placed and any camera path finished (a model that never installs is given up after 600 frames). Also skips the first launch's offer of the test assets. |
| `SPEEDO_CAPTURE` | A `.bmp` file; its folder must exist. Needs `SPEEDO_AUTOLOAD_EXIT`. Default: none. | Saves the frame the autoload exit ends on (with the interface unless `SPEEDO_UI=0`). |
| `SPEEDO_REPEATABLE` | Any value but `0` on. Only with `SPEEDO_AUTOLOAD_EXIT`. Default: off. | When the exit's count starts, restarts the frames' sequences and histories (noise, jitter, temporal histories, the indirect lighting), so that a capture is the same from run to run as far as the driver allows (see [visual testing](visual-testing-plan.md)). |
| `SPEEDO_CAPTURE_PATH` | A folder; it must exist. Default: none. | Saves `pose-NNNN.bmp` for the frame that first draws each of `SPEEDO_CAMERA_PATH`'s poses. |
| `SPEEDO_CAPTURE_PATH_FIRST` | A pose's index, from 0. Default: 0. | With `SPEEDO_CAPTURE_PATH`, captures from that pose on (e.g. after still poses let the lighting settle). |
| `SPEEDO_CAPTURE_PATH_STRIDE` | A count, at least 1. Default: 1. | With `SPEEDO_CAPTURE_PATH`, captures every n-th pose from `SPEEDO_CAPTURE_PATH_FIRST`. |

## Time and animation

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_TIME_FRAME_SECONDS` | Seconds per drawn frame. Default: real time. | Counts each frame as this long, so that runs match frame for frame: the time of day's advance, the motion blur's shutter and the indirect lighting's blending (which keeps it within 1/480 to 0.25 s). |
| `SPEEDO_ANIMATION_TIME` | Seconds. Default: animations play from 0. | Pauses each model's first animation at that time (View > Animation > Play off). |
| `SPEEDO_ANIMATION_FRAME_SECONDS` | Seconds of animation per drawn frame. Default: real time. | Advances animations by a fixed step a frame, so that two runs show moving things in the same place on the same frame. |

## The camera and lens

All of these are part of a pose. Lens glare, focus, motion blur and grain need a lens: the pinhole has none of them.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_LENS` | `pinhole`, `gauss-50`, `wide-28`, `portrait-85`, `vintage-50`, `anamorphic-50`, `anamorphic-40`, `anamorphic-75`, `anamorphic-50-133`, `wide-20`, `ultrawide-16`, `ultrawide-12`, `fast-35`, `tele-135`. Default: `pinhole`. | Pose. The lens, as View > Camera > Lens. Every lens but the pinhole is a prime, designed element by element: it sets the focal length and the apertures, and draws its vignetting, aberration and distortion. |
| `SPEEDO_SENSOR` | `full-frame`, `aps-c`, `super35`, `1-inch`, `phone`. Default: `full-frame` (36x24 mm). | Pose. The sensor's size, as View > Camera > Sensor. The focal length is kept, so a smaller sensor sees a narrower field. |
| `SPEEDO_FOCAL_LENGTH` | mm, 1 to 5000 (the field of view kept within 1 to 170 degrees). Default: the field of view as it is (75 degrees vertically). | Pose. The field of view as a focal length on the sensor, as View > Camera > Focal Length. A lens sets its own instead. |
| `SPEEDO_LENS_GLARE` | `0` off. Default: on. | Pose. The lens's glare: bloom, ghosts, starbursts, and an anamorphic lens's streaks. View > Camera > Lens Glare. |
| `SPEEDO_FOCUS` | `spot` (or `auto`), `zone`, `wide`, `manual` (at 2 m), or a distance in meters. Default: `spot`. | Pose. What the depth of field focuses on: a spot or a zone at the view's center, the nearest thing in view, or a distance. View > Camera > Focus. |
| `SPEEDO_FOCUS_CONTINUOUS` | `0` off. Default: on. | Pose. On, the autofocus measures every frame (AF-C); off, it keeps its first focus (AF-S) until Focus Now. |
| `SPEEDO_MOTION_BLUR` | `0` off. Default: on. | Pose. What moves smears over the shutter's share of the frame. View > Camera > Motion Blur. |
| `SPEEDO_GRAIN` | `0` off. Default: on. | Pose. The sensor's noise, stronger at higher ISO. View > Camera > Grain. |

## Exposure and tonemapping

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_EXPOSURE_MODE` | `program` (or `auto`), `aperture`, `shutter`, `manual`. Default: `program`. | Pose. How the camera exposes, as View > Camera > Exposure: program picks the aperture, shutter and ISO from the scene's brightness, priority fixes one of them, manual all three. Wins over `SPEEDO_AUTO_EXPOSURE`. |
| `SPEEDO_AUTO_EXPOSURE` | `0` manual. Default: program. | Pose. Turns auto exposure off: the exposure then comes from `SPEEDO_APERTURE`, `SPEEDO_SHUTTER` and `SPEEDO_ISO` (f/16, 1/160 s and ISO 100 by default), for comparing renders at one exposure. View > Auto Exposure. |
| `SPEEDO_APERTURE` | An f-number, 0.7 to 64, then within the lens's range. Default: 16. | Pose. The aperture in manual and aperture priority modes; with a lens, also the depth of field. |
| `SPEEDO_SHUTTER` | Seconds, or `1/<n>` (e.g. `1/250`); 1/8000 to 30 s. Default: 1/160 s. | Pose. The shutter in manual and shutter priority modes; with a lens, also the motion blur's length. |
| `SPEEDO_ISO` | 50 to 409600. Default: 100. | Pose. The ISO in manual mode; with a lens, also the grain. |
| `SPEEDO_EXPOSURE` | Stops (EV). Default: 0. | Pose. Exposure compensation, on top of the camera's exposure, auto or manual. View > Exposure. |
| `SPEEDO_TONEMAPPER` | `pbr-neutral`, `aces`, `agx`, `reinhard`, `linear`. Default: `pbr-neutral`. | Pose. The tonemapping curve: Khronos PBR Neutral, ACES, AgX, Reinhard, or linear (clamped). View > Tonemapper. |
| `SPEEDO_UPSCALING` | `off`, `native`, `quality`, `balanced`, `performance`. Default: `balanced`. | Pose. Temporal upscaling, as View > Upscaling: off renders at the output's size without history; native at that size, accumulated over frames (antialiasing alone); the others at 77%, 67% or 50% of it each way, upscaled. |
| `SPEEDO_EXPOSURE_LOG` | Exactly `1` on. Default: off. | Diagnostic. Prints the first view's auto exposure metering (the histogram's percentiles, the mean, the target) every frame for the first 120 metered frames, then every 30th. |

## The environment, sky and time

All of these are part of a pose. Apart from the environment's intensity and rotation they are the physical sky's, which lights the scene unless `SPEEDO_AUTOLOAD_ENVIRONMENT` loads a panorama.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_ENVIRONMENT_INTENSITY` | A multiplier, 0 or more. Default: 1. | Pose. Scales the environment's light: the sky or panorama, its sun or moon, and the air's light. View > Environment > Intensity. |
| `SPEEDO_ENVIRONMENT_ROTATION` | Degrees. Default: 0. | Pose. Turns the environment, and its sun, about the vertical axis. View > Environment > Rotation. |
| `SPEEDO_TIME_OF_DAY` | Hours of local solar time (`18.5`) or `hours:minutes` (`18:30`), 0 to 24. Default: 10.5. | Pose. Places the sun (highest at 12), the moon and the stars. View > Environment > Time of Day. |
| `SPEEDO_TIME_SPEED` | Times real time; negative runs backwards. Default: time stands still. | Pose. Advances the time of day by itself, this many times real time (60 is an hour a minute). View > Environment > Advance Time and Time Speed. |
| `SPEEDO_LATITUDE` | Degrees, north positive, -90 to 90. Default: 48.86 (Paris). | Pose. Where on Earth the sky is seen from. View > Environment > Latitude. |
| `SPEEDO_DAY_OF_YEAR` | 1 (1 January) to 365. Default: 172 (21 June). | Pose. The date: the sun's height for the season, and the moon's place. View > Environment > Day of Year. |
| `SPEEDO_YEAR` | 1900 to 2200. Default: the current year. | Pose. The year the moon's place and phase are worked out for. View > Environment > Year. |
| `SPEEDO_VISIBILITY` | km, 1 to 288.5 (the air alone). Default: 217.4 (a very clear day). | Pose. How clear the air is: the sky, the sun's light, the haze and the light shafts follow it. View > Environment > Visibility. Not the visibility buffer. |
| `SPEEDO_AERIAL_PERSPECTIVE` | `0` off. Default: on. | Pose. The air between the eye and what it sees, so that far things fade into haze; fog and light shafts need it. View > Environment > Aerial Perspective. |
| `SPEEDO_LIGHT_SHAFTS` | `0` off. Default: on. | Pose. The sun's (at night the moon's) shadows in the air. Needs aerial perspective and shadows. View > Environment > Light Shafts. |
| `SPEEDO_FOG` | `0` off. Default: on (a clear day's light haze). | Pose. Haze near the ground, thickest at it. Needs aerial perspective. View > Environment > Fog. |
| `SPEEDO_FOG_VISIBILITY` | km at the ground, 0.05 to 1000. Default: 30. | Pose. How far the fog lets you see at the ground: 30 a clear day, 2 mist, 0.2 fog. View > Environment > Fog Visibility. |
| `SPEEDO_FOG_HEIGHT` | m, 1 to 5000. Default: 200. | Pose. How high the fog reaches: at this height it is about a third as dense as at the ground. View > Environment > Fog Height. |

## Lighting, shadows and ray tracing

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_SHADOWS` | `0` off. Default: on. | Pose. The lights' shadows from the shadow atlas; off also stops the traced shadows. View > Shadows. |
| `SPEEDO_TRACED_SHADOWS` | `0` off, `all` traced at every pixel. Default: on, traced where the atlas is unsure. | Pose. Traces the brightest lights' shadows (up to 4) through the BVHs, softened by the lights' sizes; `all` is for comparisons. View > Ray Traced Shadows. |
| `SPEEDO_TRACED_REFLECTIONS` | `0` off. Default: on, where the device has ray queries. | Pose. Traces what smooth surfaces and clear coats reflect. View > Ray Traced Reflections. |
| `SPEEDO_TRACED_REFRACTIONS` | `0` off. Default: on, where the device has ray queries. | Pose. Traces what transmissive surfaces show behind them along the refracted ray. View > Ray Traced Refractions. |
| `SPEEDO_CAUSTICS` | `0` off. Default: on, where the device has ray queries. | Pose. Refracts the lights' light through still transmissive surfaces into caustics; off, it passes straight through, tinted. View > Caustics. |
| `SPEEDO_EMISSIVE_LIGHTS` | `0` off. Default: on. | Pose. Emissive surfaces light the scene as area lights; off, only the indirect lighting's rays see their light. View > Emissive Lights. |
| `SPEEDO_LIGHT_SIZES` | `<light>=<size>` pairs separated by commas: the light's index (`#n` in View > Lights...), and a directional light's angular radius in degrees or a point or spot light's radius in scene units. Default: the sun's size for directional lights, 0 for the others. | The model's lights' sizes, which soften their shadows, as the Lights window's Radius. Applied once a model is in. |
| `SPEEDO_MERGE_LIGHTS` | `0` off. Default: on. | Takes the environment's sun and a model's directional light within 5 degrees of it as one light, the model's. View > Environment > Merge with the model's lights. |
| `SPEEDO_CLUSTERED_LIGHTS` | `0` off. Default: on. | Each pixel loops over the lights that may reach its cluster rather than all of them. View > Clustered Lights. |
| `SPEEDO_SHADOW_CACHE` | `0` off. Default: on. | Keeps the shadow atlas's tiles while their views don't change; off redraws every shadow view every frame. |

On macOS ray queries are opt in (`SPEEDO_RAY_QUERY=1`, see [the gpu and drivers](#the-gpu-and-drivers)): without them the traced reflections, refractions and caustics are off.

## Indirect lighting

`SPEEDO_GI` and the variables marked Pose are View settings; the rest is the renderer's tuning, whose defaults are what [indirect lighting](notes/indirect-lighting.md) measured.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_GI` | `0` off. Default: on. | Pose (always written). Indirect lighting: irradiance probes over a voxel clipmap. View > Indirect Lighting. |
| `SPEEDO_GI_RESOLUTION` | Voxels along the scene's longest axis, 8 to 512. Default: 128. | Pose. The last clip's resolution, which the finer clips follow: finer resolves light through narrower openings at a steep cost. View > Indirect Lighting Resolution. |
| `SPEEDO_GI_TRACE` | `voxels`, or anything else for triangles. Default: triangles. | Pose. What the rays meet: the model's triangles through its BVHs (where it has them), or the voxels. The near field needs triangles. View > Indirect Lighting Rays. |
| `SPEEDO_GI_CLIP_COUNT` | 1 to 4. Default: 3. | The clipmap's levels: the last covers the scene, each finer one has voxels half the size and follows the eye (a clip per split screen view). |
| `SPEEDO_GI_LOOK_AHEAD` | A share of a clip's half size, 0 to 1. Default: 0.5. | How far ahead of the eye, along where it looks, the finer clips are centered. |
| `SPEEDO_GI_PROBE_SPACING` | Voxels: 8, 16, 24 or 32 (another value fails at startup). Default: 8. | The probes' spacing, in each clip's voxels. |
| `SPEEDO_GI_EMISSION_REACH` | Probe spacings, 0 to 8 (0 no limit). Default: 0.5. | A probe's ray meeting an emissive surface nearer than this takes its light as from this far, so that a lamp beside a probe doesn't flood its cell. Only with emissive lights off. |
| `SPEEDO_GI_BOUNCE` | `0` off. Default: on. | The voxels reflect the probes' last light, so that light bounces on, a bounce a frame; off, they hold the lights' direct light alone. |
| `SPEEDO_GI_RAY_COUNT` | Rays per probe, 8 to 1024. Default: 128. | The most rays a probe traces an update. |
| `SPEEDO_GI_RAY_BUDGET` | Rays per frame for all probes, at least 1. Default: 500000. | Divides among the probes traced, lowering their rays (to 16 at least). |
| `SPEEDO_GI_RAY_PHASE_COUNT` | Updates, 1 to 256. Default: 32. | Each update turns the probes' ray directions; after this many they repeat. |
| `SPEEDO_GI_HYSTERESIS` | 0 to 0.99. Default: 0.97. | The share of its light a probe keeps a frame (at 60 fps where the blending is time based). |
| `SPEEDO_GI_TIME_BASED` | `0` off. Default: on. | Blends by the time elapsed rather than per frame, and lets the surface cache's window grow and settle. |
| `SPEEDO_GI_ADAPT` | Exactly `1` on. Default: off. | Probes blend faster through a change of light far beyond their noise (and never freeze). |
| `SPEEDO_GI_PROBE_FREEZE` | `0` off. Default: on. | Once the scene has been still a while, a settled probe keeps its light rather than tracing, every second cycle. |
| `SPEEDO_GI_STAGGER` | `0` off. Default: on. | The finest clips update every frame and the coarser levels take turns; off updates every clip every frame. |
| `SPEEDO_GI_RELIGHT_PHASE_COUNT` | Updates, 1 to 64. Default: 4. | When the lights change gradually (the sun moving), the static voxels are lit again over this many updates. |
| `SPEEDO_GI_CACHE` | `0` off. Default: on. | Pose. The surface cache: rays from each voxel, accumulated and filtered along surfaces, which resolve light at the voxels' size. View > Indirect Lighting Surface Cache. |
| `SPEEDO_GI_CACHE_RAYS` | Rays per voxel an update, 1 to 64. Default: 1. | The cache's rays (at least 4 for its first 256 updates, where the blending is time based). |
| `SPEEDO_GI_CACHE_SAMPLES` | 1 to 65535. Default: 1024. | The most samples a voxel's average covers. |
| `SPEEDO_GI_CACHE_FILTER_PASSES` | 1 to 4. Default: 3. | The filter's passes along surfaces. |
| `SPEEDO_GI_CACHE_SIGMA` | 0 or more. Default: 8. | How many deviations of the noise a neighbour's light may differ by and still be averaged in. |
| `SPEEDO_GI_CACHE_SETTLED_FRAMES` | Updates. Default: twice `SPEEDO_GI_CACHE_SAMPLES`. | After this many updates the cache is settled, and its voxels trace every other frame. |
| `SPEEDO_GI_CACHE_KEEP_FRAMES` | Frames. Default: 1024. | How long a block of voxels keeps its cache page after a lookup last read it. |
| `SPEEDO_GI_CACHE_EVERYWHERE` | Exactly `1` on. Default: off. | Traces and filters every voxel near a surface, not only those around what the view reads: for comparisons. |
| `SPEEDO_GI_NEAR` | `0` off. Default: on. | Pose. The near field: near other surfaces, short rays from the pixels light them instead of the voxels, so that contacts darken smoothly. Needs triangles. View > Indirect Lighting Near Field. |
| `SPEEDO_GI_NEAR_REACH` | Voxels, 0 to 64 (0 off). Default: 8. | How far the near field's rays go. |
| `SPEEDO_GI_NEAR_CONTACT` | Voxels, 0 to 8 (and within the reach). Default: 2.5. | Pose. How near a hit is a contact, where the rays' light replaces the voxels'. View > Near Field Contact. |
| `SPEEDO_GI_NEAR_POWER` | 0 to 64. Default: 2. | The voxels' weight against the rays' light, the share of rays that met no contact to this power. |
| `SPEEDO_GI_NEAR_STRIDE` | `2`, or anything else for 4. Default: 4. | Pose. Traces the near field from every 2nd or 4th pixel each way. View > Near Field Rays. |
| `SPEEDO_GI_NEAR_TEMPORAL` | `0` off. Default: on. | Pose. Accumulates the near field's rays over frames, reprojected. View > Near Field Over Frames. |
| `SPEEDO_GI_NEAR_HISTORY_FRAME_COUNT` | Frames, 1 to 1024. Default: 16. | About how many frames the near field's history holds. |
| `SPEEDO_GI_NEAR_STILL_FRAME_COUNT` | Frames, 1 to 2048. Default: 256. | A longer history while the view, the model and the lights are still. |
| `SPEEDO_GI_DEBUG` | 1 to 14. Default: none. | Diagnostic. Starts in that indirect lighting debug view (View > Debug View > Indirect Lighting, in its order); `SPEEDO_DEBUG_VIEW` wins. |
| `SPEEDO_GI_REFERENCE_RAYS` | Rays per pixel, 16 to 4096. Default: 256. | Diagnostic. The Surface Reference debug view's rays; lower it where a frame of it times the gpu out. |
| `SPEEDO_GI_CACHE_SHOW` | Exactly `1` on. Default: off. | Diagnostic. The voxels show the surface cache's light alone where it has some. |
| `SPEEDO_GI_NEAR_SHOW` | 0 to 4. Default: 0. | Diagnostic. Shows the near field: 1 the rays' light, 2 the share of rays without a contact, 3 the light of rays that met something, 4 of those that didn't. |
| `SPEEDO_GI_DUMP` | `x,y,z` (a world position) or `pixel:x,y`. Default: off. | Diagnostic. At exit, prints each clip's probe nearest the point (its rays, light and resolve); every clip then updates every frame. |
| `SPEEDO_GI_EYE_PATH` | Points `x,y,z[,lx,ly,lz]` separated by `;` or new lines, or `@<file>`. Default: the camera. | Diagnostic. Centers the clips on these points, one a frame, then the last: the clips' movement under a still camera, or a replay of `SPEEDO_GI_EYE_LOG`. |
| `SPEEDO_GI_EYE_LOG` | Exactly `1` on. Default: off. | Diagnostic. Prints each frame's clip center and look as `SPEEDO_GI_EYE_PATH` takes them, and when a clip scrolls. |
| `SPEEDO_GI_REVOXELIZE` | Exactly `1` on. Default: off. | Diagnostic. Voxelizes every clip again every frame, starting the probes over: to compare with what scrolling the clips gives. Slow. |

## Emissive surfaces

The renderer's tuning of the emissive surfaces' light (`SPEEDO_EMISSIVE_LIGHTS`), for a model with emitters and BVHs.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_EMITTER_STRIDE` | 1, 2, 4 or 8. Default: 4. | Gathers the light at every n-th pixel each way. |
| `SPEEDO_EMITTER_CANDIDATE_COUNT` | 1 to 256. Default: 8. | The candidate points a traced pixel resamples a frame before its shadow ray. |
| `SPEEDO_EMITTER_VOXEL_SAMPLE_COUNT` | 1 to 256. Default: 4. | The samples, each with a shadow ray, averaged for an indirect lighting voxel. |
| `SPEEDO_EMITTER_VOXEL_CANDIDATE_COUNT` | 1 to 256. Default: 8. | The candidates resampled for each of a voxel's samples. |
| `SPEEDO_EMITTER_REUSE` | Exactly `1` on. Default: off. | Reuses reservoirs, last frame's at the same surface and the neighbours'. |
| `SPEEDO_EMITTER_HISTORY_CAP` | 1 to 1000, in candidate counts. Default: 20. | How much last frame's reservoir may count, with reuse. |
| `SPEEDO_EMITTER_SPATIAL_COUNT` | 0 to 8. Default: 0. | The neighbours a pixel merges reservoirs with, with reuse. |
| `SPEEDO_EMITTER_FILTER_REACH` | Pixels, 1 to 64. Default: 16. | How far the light's filter along rows and columns reaches. |
| `SPEEDO_EMITTER_HISTORY_FRAME_COUNT` | Frames, 1 to 1024. Default: 32. | About how many frames the light is averaged over. |
| `SPEEDO_EMITTER_STILL_FRAME_COUNT` | Frames, 1 to 2048. Default: 256. | A longer average while the camera and the model are still. |
| `SPEEDO_EMITTER_VISIBILITY_FRAME_COUNT` | Frames, 1 to 1024. Default: 8 (a quarter of it for a model that moves). | While the camera is still, how long a point found visible is trusted before its shadow ray is traced again. |
| `SPEEDO_EMITTER_ROULETTE` | 0 or more, on screen brightness. Default: 0.02. | Below it a shadow ray is traced only by chance, in proportion; 0 traces every one. |

## Culling and drawing

The renderer's, but for `SPEEDO_CULLING`, which is View > Culling's starting state.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_CULLING` | `0` off. Default: on. | Leaves out the draws outside each view's and shadow view's frustum; off also turns off occlusion culling. View > Culling. |
| `SPEEDO_INDIRECT_DRAWS` | `0` off. Default: on. | Records each run of draws with the same state as one indirect draw; off records direct draws, without gpu or occlusion culling. |
| `SPEEDO_GPU_CULLING` | `0` off. Default: on. | The gpu culls the draws; off leaves the frustum culling to the cpu, without occlusion culling. |
| `SPEEDO_OCCLUSION_CULLING` | `0` off. Default: on. | Leaves out the main view's draws hidden behind others, against a depth pyramid. Needs gpu culling. |
| `SPEEDO_OCCLUSION_MODE` | `two-phase`, `reprojected` or `hybrid`. Default: `two-phase`. | How hidden draws are found: last frame's visible draws then the rest (two phases), last frame's depth reprojected, or both. |
| `SPEEDO_SHADOW_OCCLUSION_CULLING` | Exactly `1` on. Default: off. | Occlusion culls the shadow views too (they hide little, and the pyramid costs 0.5 to 1 ms). |
| `SPEEDO_VISIBILITY_BUFFER` | `0` off. Default: on. | Shades opaque surfaces from the pre-pass's visibility buffer rather than drawing them again. |
| `SPEEDO_SHADER_TIERS` | `0` off. Default: on. | Draws materials without extended layers (clearcoat, sheen, transmission, ...) with pipelines that skip them; off draws every material with the generic ones, for comparisons. |
| `SPEEDO_BVH_BUILD` | `fast`, or anything else for spatial splits. Default: spatial splits. | How the static BVHs are built: with spatial splits (tinybvh's SBVH: rays visit fewer nodes), or binned SAH alone. Each is cached apart. |

## The window and presentation

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_HEADLESS` | Exactly `1` on. Default: off. | Runs without a display: no window, surface or swapchain; the frames are drawn into an image of `SPEEDO_WINDOW_SIZE`, which `SPEEDO_CAPTURE` saves. For locked screens and machines without a display. |
| `SPEEDO_WINDOW_SIZE` | `<width>x<height>` in pixels. Default: 1280x720 in the window system's units (2560x1440 pixels on a 2x display). | The window's framebuffer in pixels, the same on every display; headless, the image's size. |
| `SPEEDO_BACKGROUND` | Exactly `1` on. Default: off. | Opens the window behind the others without taking the focus, for automated runs (on Linux, unfocused only), and skips the offer of the test assets. |
| `SPEEDO_FULLSCREEN` | `exclusive`, `borderless`, or `native` (macOS). Default: `exclusive`. | How `Alt+Enter` goes fullscreen, as View > Presentation; it doesn't start fullscreen. |
| `SPEEDO_PRESENT_MODE` | `fifo`, `fifo_relaxed`, `mailbox`, `immediate` (anything else is fifo). Default: `fifo`. | The swapchain's present mode, as View > Presentation; `immediate` uncaps the frame rate. |
| `SPEEDO_SWAPCHAIN_IMAGE_COUNT` | A count, within the surface's limits and 8. Default: 3. | The swapchain's images, as View > Presentation. |
| `SPEEDO_PRESENTATION_TEST` | Steps separated by `;`: `fullscreen`, `fullscreen=exclusive\|borderless\|native`, `present=<mode>`, `images=<count>`, `size=<width>x<height>`, `monitor=<index>`. Default: none. | Diagnostic. Takes a step every `SPEEDO_PRESENTATION_STEP_FRAMES` frames, through View > Presentation and `Alt+Enter`, and fails if no frame is drawn for 10 s (`assettest.ps1 -Presentation`). |
| `SPEEDO_PRESENTATION_STEP_FRAMES` | Frames, at least 1. Default: 20. | The frames between the presentation test's steps. |

## The interface

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_UI` | `0` off. Default: on. | `0` draws the frames without the interface, for captures of the scene alone; the interface still runs (the autoloads, the camera, the captures). |
| `SPEEDO_SHOW_ABOUT` | Exactly `1` on. Default: off. | Opens About Speedo at startup. |
| `SPEEDO_SHOW_LIGHTS` | Exactly `1` on. Default: off. | Opens View > Lights... at startup. |
| `SPEEDO_SHOW_STATISTICS` | Exactly `1` on. Default: off. | Opens View > Statistics... at startup (debug and profile builds). |
| `SPEEDO_OPEN_MENUS` | Menu labels separated by `/`, the parents too (e.g. `View/Presentation`). Default: none. | Holds those menus open, for screenshots of them. |

## The file browser

`SPEEDO_BROWSE` opens File > Open... at startup; the others set its boxes and what it does once it lists the folder.

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_BROWSE` | A folder. Default: closed (it opens in the test assets, else the resource directory). | Opens the file browser in that folder at startup. |
| `SPEEDO_BROWSE_ENVIRONMENT` | Exactly `1` on. Default: off. | Opens the environment browser instead (View > Environment > Open Environment...): every panorama below the folder. |
| `SPEEDO_BROWSE_RECURSIVE` | `0` off. Default: on. | Include subfolders: every file below the folder rather than its own entries. |
| `SPEEDO_BROWSE_KINDS` | Any of `models`, `archives`, `images`, `panoramas`. Default: models and archives. | The kinds of file listed. |
| `SPEEDO_BROWSE_GROUND` | `0` off. Default: on. | Whether the models it opens stand on a ground plane. |
| `SPEEDO_BROWSE_SELECT` | Entries as the browser lists them, separated by `;`. Default: none. | Selects them once the folder is listed, previewing the last. |
| `SPEEDO_BROWSE_OPEN` | `as-authored`, or anything else for side by side. Default: none. | Opens the selection once it is listed, as the Open button does. |
| `SPEEDO_BROWSE_OPEN_FRAME_COUNT` | Frames. Default: 0. | Waits this many frames after the preview starts loading before opening (a double click), to test giving up a preview. |

## Test assets

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_TEST_ASSETS` | A folder. Default: `resources/test-assets` in a checkout (or under `-r`), else `test-assets` in the user's data. | Where the test assets are: where File > Open... starts and File > Download Test Assets... downloads to. The scripts read it too (`fetch-test-assets.ps1`, `visualtest.ps1`). |
| `SPEEDO_DOWNLOAD_TEST_ASSETS` | `offer`, `quick` or `full`. Default: the first launch offers them. | `offer` opens the Test Assets dialog; `quick` or `full` downloads that set, prints a summary and exits. |

## The gpu and drivers

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_RAY_QUERY` | macOS: exactly `1` on; elsewhere `0` off. Default: off on macOS, on elsewhere. | Hardware ray queries where the device has them; without, the shaders' BVHs trace the rays. Opt in on macOS while Metal's ray tracing restarts the gpu now and then (see [indirect lighting](notes/indirect-lighting.md)), on the Vulkan backend (KosmicKrisp) and the Metal backend alike (the `<triplet>-metal` preset, docs/backends-plan.md); with Vulkan it also sets `MESA_KK_EXPERIMENTAL=ray_query` for KosmicKrisp. |
| `SPEEDO_EXECUTOR_THREAD_COUNT` | Threads, at least 1. Default: the hardware's threads less 2. | The task executor's worker threads: `2` reproduces a small machine or a CI runner. |
| `SPEEDO_GPU_HANG_TIMEOUT` | Seconds; 0 (or text) never. Default: 20. | How long a gpu wait, or the draw thread without a heartbeat, may last before the client writes a hang report (in the profile's `crashes`) and exits. |

## Diagnostics

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_DEBUG_VIEW` | A debug view's name or number (below). Default: the lit image. | Pose. Shows a surface's values, a part of the light, or the indirect lighting's insides instead of the lit image. View > Debug View. |
| `SPEEDO_GPU_TIMINGS` | Exactly `1` on. Default: off. | At exit, prints the median frame time, each gpu pass's median, the occlusion culling's draws and the indirect lighting's statistics. On KosmicKrisp measure the draw thread's frame time: its timestamps miss compute passes. |
| `SPEEDO_SKIP_PASSES` | The frame graph's pass names, exact, separated by `;` (e.g. `GI Near;Emitter Shade`; inside the GI pass also its steps `GI Light`, `GI Flags`, `GI Trace`, `GI Probe Update`, `GI Cache Release`, `GI Resolve`, `GI Cache Filter`). Default: none. | Leaves those passes out, to measure what they cost by the frame time. The image is then wrong. |
| `SPEEDO_LOG_PIPELINES` | Exactly `1` on. Default: off. | Logs every pipeline made and how long it took; without it, only those taking a second or more. |
| `SPEEDO_SHADER_LOG` | Exactly `1` on. Default: off. | Prints the shader compiler's details: paths, reflection, bindings, included files (a checkout's startup compile; an install's shaders are precompiled). |
| `SPEEDO_CRASH_BREADCRUMBS` | `markers`, `checkpoints` or `none`. Default: markers where the device has them, else checkpoints. | Which breadcrumbs the crash reports' passes in flight come from (`VK_AMD_buffer_marker` or `VK_NV_device_diagnostic_checkpoints`). |
| `SPEEDO_CRASH_TEST` | `hang`. Default: off. | From the 100th frame, waits on a semaphore nothing signals, so that the client reports a hang: with a short `SPEEDO_GPU_HANG_TIMEOUT`, tests the crash reports. |
| `SPEEDO_RAY_QUERY_WATCH` | Any value but `0` on. Default: off. | Gives each dispatch a breadcrumb and records the ray queries in flight, so that a hang's report shows what they traced. |
| `SPEEDO_RAY_QUERY_REPLAY` | A text file of rays, one a line: origin, direction, tmax, any hit (0 or 1), moving (0 or 1). Default: off. | Traces those rays every frame after the indirect lighting (e.g. those a hang's report listed), to find one that hangs the gpu. Turns the watch on. |
| `SPEEDO_RAY_QUERY_REPLAY_REPEAT` | A count. Default: 64. | How many times a frame each replayed ray is traced. |
| `SPEEDO_VALIDATE_SYNC` | Set (to anything, even `0`) on. Default: off. | The validation layer's synchronization checks, which are slow: in a debug build, and in the scripts' `-Validate` runs. |
| `SPEEDO_VALIDATION_BREAK` | `error`, `warning`, or `off`. Default: off. | Breaks into an attached debugger on validation messages of that severity or worse (debug builds). |

The debug views, by name (and number): `base-color` (1), `normal` (2), `geometry-normal` (3), `roughness` (4), `metallic` (5), `occlusion` (6), `material` (7), `texcoord` (8), `shadows` (9), `light-count` (10), `emissive` (11), `direct` (12), `indirect` (13), `indirect-diffuse` (14), `indirect-specular` (15), `gi-irradiance` (16), `gi-visibility` (17), `gi-voxel-behind` (18), `gi-voxel-front` (19), `gi-voxel-albedo` (20), `gi-reference` (21), `gi-clip` (22), `gi-grid` (23), `gi-voxel-world` (24), `gi-voxel-world-albedo` (25), `gi-voxel-world-normal` (26), `gi-probes` (27), `gi-surface-reference` (28), `gi-probe-state` (29), `traced-shadow` (30), `traced-shadow-raw` (31), `traced-shadow-reference` (32), `emitters` (33), `emitters-raw` (34), `emitters-reference` (35), `traced-reflections` (36). The `gi-*` views need indirect lighting; the references take seconds a frame. An unknown name isn't understood, as for any pose variable.

## The scripts and the build

| Variable | Values (default) | What it does |
|---|---|---|
| `SPEEDO_BINARY_CACHE` | `0` off. Default: on. | Setup and vcpkg read the shared binary cache, and FASTBuild's cache plugin its remote; `0` keeps both local (see [build](notes/build.md), "vcpkg's shared binary cache"). |
| `SPEEDO_BINARY_CACHE_WRITE` | Set to anything: on. Default: off. | Also writes what this machine builds to the shared caches, vcpkg's packages and FASTBuild's objects, with the AWS credentials in the environment (`AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`, or `AWS_PROFILE`); setup then installs the AWS CLI. |
| `SPEEDO_FASTBUILD_CACHE` | `0` off. Default: on. | The presets' FASTBuild launcher runs FBuild with its cache, local and remote; `0` runs it as asked (see [build](notes/build.md), "FASTBuild's cache"). |
| `FASTBUILD_CACHE_MODE` | FASTBuild's own (`r`, `w`, `rw`). Default: unset. | Set, the launcher leaves the cache's mode to FBuild rather than adding `-cache`. |
| `ASSETTEST_CLIENT_TIMEOUT` | Seconds. Default: 600. | How long `assettest.ps1` gives each client run. |

`SPEEDO_TEST_ASSETS` and `SPEEDO_VALIDATE_SYNC` above are the scripts' too. Variables like `SPEEDO_TARGET_CPU` and `SPEEDO_SANITIZE` are CMake's, set by the presets, not the environment's.

## The Vulkan loader

| Variable | What it does |
|---|---|
| `VK_DRIVER_FILES` | The driver's manifest: KosmicKrisp's on macOS from a checkout, or lavapipe's to render without a gpu (see the [README](../README.md#3-run-the-client)). |
| `VK_LAYER_PATH` | Where the validation layer's manifest is, for a debug build from a shell: `install/<triplet>/share/vulkan/explicit_layer.d` (`bin` on Windows). The scripts' `-Validate` set it. |
| `VK_LOADER_DEBUG` | The loader's own logging (`all`, `error`, ...). A debug build prints it, `VK_LAYER_PATH` and `VK_DRIVER_FILES` at startup. |
| `MESA_KK_EXPERIMENTAL` | KosmicKrisp's experimental features; the client sets `ray_query` where ray queries are on (macOS). |
