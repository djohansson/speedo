# speedo

speedo is a real-time 3D renderer and asset viewer built on Vulkan. Open a glTF or OBJ model, a zip archive of models,
an HDR environment or an image, and fly around it: materials, lights, shadows, transparency and animation are drawn as
the glTF spec and the Khronos sample viewer describe them, and big scenes stay fast through culling and clustered
lighting. It is also a playground for engine architecture: a task system, a frame graph, a graphics abstraction layer and
a client/server split.

It runs on macOS (Apple Silicon, through KosmicKrisp or MoltenVK), Windows and Linux, and builds with clang everywhere.

![CarConcept: a clearcoated concept car with transmissive glass, on a rooftop helipad at sunset](docs/screenshots/carconcept.jpg)

![ChronographWatch: a gold watch with a glass face, in the forecourt of a theatre at dusk](docs/screenshots/chronographwatch.jpg)

![DamagedHelmet: a worn sci-fi helmet with an emissive visor, in a ruin overgrown with greenery](docs/screenshots/damagedhelmet.jpg)

*Khronos glTF sample models CarConcept, ChronographWatch and DamagedHelmet, lit by the helipad, footprint_court and
papermill panoramas from glTF-Sample-Environments.*

<!-- made with the profile client: SPEEDO_AUTOLOAD_MODEL=test-assets/glTF-Sample-Assets/Models/<model>/glTF/<model>.gltf,
SPEEDO_AUTOLOAD_ENVIRONMENT=test-assets/glTF-Sample-Environments/<panorama>.hdr, SPEEDO_ANIMATION_TIME=0 and
SPEEDO_CAMERA=2.312,1.522,2.892,15.141,-41.082 (CarConcept), 2.210,1.534,3.820,14.439,-20.556 (ChronographWatch),
1.168,0.273,1.448,7.723,-35.538 (DamagedHelmet); the window captured once the ui had faded, without its title bar and
rounded corners, scaled to 1600 wide -->

## What's in the box

| Program     | What it does |
|-------------|--------------|
| `client`    | The viewer. A window with a menu bar, one or more views of the loaded scene, and tools for inspecting it, in a translucent, frosted interface styled after macOS's. |
| `server`    | A small companion process the client talks to over ZeroMQ. It is the start of a distributed task system and does little yet. |
| `assettest` | A command line tool that imports every model, image and environment it's given and checks the results. It's the main automated test. |
| `filebench` | A benchmark of the ways to read a file (std streams, stdio, memory mapping), warm and cold, of the importers (`--import <files>`) and of zip extraction (`--zip <archives>`). |

### Using the client

- **File > Open...** opens a file browser with thumbnails for models (`.gltf`, `.glb`, `.obj`), zip archives of
  models, environments (`.hdr`) and images: it previews the model, zip archive (its models side by side), image or
  panorama you click (or step to with the arrow keys), can list every file below a folder at once ("Include subfolders")
  filtered by kind (models and archives by default), and opens one or several files (several models side by side, or as
  authored when they are parts of one scene).
- **Moving around:** hold the left mouse button and drag to look around, use `W`/`A`/`S`/`D` to move, and the scroll
  wheel to change the move speed. `Alt+Enter` toggles fullscreen.
- **The interface fades away** when it isn't in use, leaving the scene: move the mouse to the top of the window to bring
  the menu bar back. Open windows and menus keep it up; the loading bars and the FPS overlay always show.
- **View** is where most of the settings live:
  - the views layout (`Ctrl+1` to `Ctrl+4`: one view, two side by side or stacked, or four)
  - presentation: the fullscreen mode (exclusive, borderless, or on macOS a space of its own), and the swapchain's
    present mode and image count, of those the display offers
  - the environment: the procedural sky or a panorama, its rotation and intensity, and its dominant lights
  - the glTF scene, material variant, animation and camera
  - exposure and tonemapping
  - shadows, culling and clustered lighting, each of which can be turned off to compare
  - Lights..., which lists the lights and draws them over the views (their direction, cones, ranges and shadow views)
  - FPS and Statistics..., including GPU timings per render pass (debug and profile builds)
- By default the views start on the model's first camera, or framed on the model if it has none.

### What it renders

- **Models:**
  - glTF 2.0, including compressed meshes (Draco, meshopt) and compressed textures (KTX2/Basis, WebP)
  - Wavefront OBJ with MTL materials
- **Materials:** metallic-roughness, specular-glossiness and unlit, plus every `KHR_materials_*` extension used by the
  Khronos sample models: clearcoat, sheen, transmission, volume, iridescence, anisotropy and more.
- **Lighting:**
  - image based lighting from the environment
  - glTF punctual lights, plus the environment's strongest light sources as directional lights, binned per view into
    clusters so that each pixel only shades the lights that can reach it
  - shadows from one atlas: cascades for directional lights, and every point and spot light in view, sized by how big
    it looks
- **Transparency:** exact, order independent.
- **Animation:** skins, morph targets, node animations and `KHR_animation_pointer`.
- **Culling:** models are cut into spatial chunks, which each view and each shadow view skips when they are out of
  sight.

## Getting started

### 1. Set up the machine

Run the bootstrap script for your platform from the repository root:

| Platform | Command |
|----------|---------|
| macOS    | `./setup_osx.sh` |
| Linux    | `./setup_linux.sh` |
| Windows  | `setup_windows.bat` |

The bootstrap script installs PowerShell 7.5+ if it's missing, then hands over to `setup.ps1`, which:

- installs the system packages: Homebrew, apt or winget, plus Visual Studio on Windows, and on macOS the Vulkan drivers
  (`mesa` for KosmicKrisp and `molten-vk`)
- bootstraps the `vcpkg` submodule
- generates `CMakeUserPresets.json` and the VS Code launch and task configurations

Re-run `setup.ps1` after OS or SDK updates; it notices them and clears stale CMake caches.

The first configure builds every dependency through vcpkg, including the LLVM toolchain itself, so expect it to take a
long time. Later configures restore packages from vcpkg's binary cache.

### 2. Configure and build

Every build directory comes from a preset named `<triplet>-<config>`, for example `arm64-osx-clang-debug`. There are
three configs:

| Config    | Use it for |
|-----------|------------|
| `debug`   | Day-to-day debugging. Vulkan validation layers and asserts are on, and it's slow. |
| `profile` | Optimized, with profiling (Tracy and GPU pass timings) and asserts. The best default for testing. |
| `release` | Optimized, without profiling. |

```sh
cmake --preset arm64-osx-clang-profile
cmake --build build/arm64-osx-clang-profile
```

Always configure with `cmake --preset <name>` before building from a shell; don't copy raw `cmake` commands out of an
IDE log. The presets carry the toolchain environment, and without it the build fails in confusing ways. CLAUDE.md
explains why.

In VS Code, the CMake Tools extension picks up the presets, and the generated launch configurations start the client,
the server or the Tracy profiler with the right environment.

### 3. Run

Run the programs from the repository root:

```sh
./build/arm64-osx-clang-profile/client
```

| Option | Default |
|--------|---------|
| `-r <dir>`: the resource directory (fonts, shaders, test assets) | `resources/` |
| `-u <dir>`: the user profile directory (settings, the asset and pipeline caches, logs) | `.speedo/` |

On macOS, point the Vulkan loader at a driver, for example KosmicKrisp:

```sh
export VK_DRIVER_FILES=$(brew --prefix mesa)/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json
```

For a debug build from a shell, also set `VK_LAYER_PATH=$PWD/install/<triplet>/share/vulkan/explicit_layer.d` so the
validation layers are found. The VS Code launch configurations set both.

## Testing

### Test assets

The test sets are downloaded from their original sources rather than kept in the repository. This command fetches
everything into `resources/test-assets/` (gitignored), or into `$SPEEDO_TEST_ASSETS` if that is set:

```sh
pwsh scripts/fetch-test-assets.ps1
```

- **Morgan McGuire's Computer Graphics Archive:** OBJ scenes, about 2.7 GB
- **The Khronos glTF-Sample-Assets models:** about 2.3 GB
- **The Khronos glTF-Sample-Environments panoramas:** about 400 MB

Downloads are pinned to a commit or checked against the checksums in `scripts/test-assets/`. Re-running the script
only fetches what's missing or changed. It prints the paths to test, which also include the hand-made models in
`resources/test-models/` for cases no downloaded asset covers.

### The asset regression

`scripts/assettest.ps1` is the main test suite. Given zip archives or directories, it runs `assettest` over every
model, image and environment in them. `assettest` checks index ranges, normals, winding, textures, mip chains,
compression error, environment prefiltering and more. With `-Client`, the script also opens each model in the client
and fails on load errors, asserts and validation messages.

From pwsh:

```powershell
scripts/assettest.ps1 -Client -Preset arm64-osx-clang-profile -Work <work dir> (scripts/fetch-test-assets.ps1)
```

From another shell:

```sh
pwsh scripts/assettest.ps1 -Client -Preset arm64-osx-clang-profile -Work <work dir> $(pwsh scripts/fetch-test-assets.ps1)
```

- **The work directory** holds the extracted archives and the asset caches. Reuse it between runs: the first run
  imports everything and is slow, later runs are much faster.
- **The full regression:** run the `profile` preset first, then `debug` with `-ClientOnly` and the same `-Work`
  directory. Validation only runs in debug, which imports slowly, so it reuses the caches profile filled.
- **Speed:** `assettest` uses every CPU thread, and the script runs four clients at once (`-Jobs <n>` to change it).
  For quick checks while iterating, `scripts/fetch-test-assets.ps1 -Quick` prints a smaller set, a model or two per
  feature (`scripts/test-assets/quick.txt`), which runs in about a minute:
  `scripts/assettest.ps1 -Client -Preset arm64-osx-clang-profile -Work <work dir> (scripts/fetch-test-assets.ps1 -Quick)`.
- **Background windows:** the client windows it opens stay behind your other windows and don't take focus, so you can
  keep working while it runs.
- **Results:** each group of checks ends in a pass/warn/fail summary. Warnings are known problems in the source assets.
  Failures and client asserts are regressions. Each client run's log is kept in `<work dir>/logs/<preset>/`, so a
  profile failure can still be read after the debug run.

### Driving the client from scripts

Environment variables let you start the client in a known state, which is handy for reproducing bugs and taking
screenshots:

| Variable | Effect |
|----------|--------|
| `SPEEDO_AUTOLOAD_MODEL`, `SPEEDO_AUTOLOAD_IMAGE`, `SPEEDO_AUTOLOAD_ENVIRONMENT` | Load these files at startup (absolute, or relative to the resource directory). |
| `SPEEDO_AUTOLOAD_SCENE=<index>`, `SPEEDO_AUTOLOAD_VARIANT=<index>` | Load the model's glTF scene, or `KHR_materials_variants` variant, of that index. |
| `SPEEDO_AUTOLOAD_EXIT=<frames>` | Exit that many frames after the loads finish. |
| `SPEEDO_BACKGROUND=1` | Open the window behind the others, without taking focus. |
| `SPEEDO_CAMERA=x,y,z,pitch,yaw` | Place the views once the loads are installed (degrees; positive pitch looks down, positive yaw turns from -z towards +x). For repeatable measurements. |
| `SPEEDO_ANIMATION_TIME=<seconds>` | Freeze animations at a fixed time. |
| `SPEEDO_TONEMAPPER`, `SPEEDO_AUTO_EXPOSURE`, `SPEEDO_SHADOWS`, `SPEEDO_CULLING`, `SPEEDO_CLUSTERED_LIGHTS`, `SPEEDO_MERGE_LIGHTS`, `SPEEDO_ENVIRONMENT_ROTATION`, `SPEEDO_ENVIRONMENT_INTENSITY` | Start with these view settings (`0` turns a setting off). |
| `SPEEDO_FULLSCREEN=exclusive\|borderless\|native` | The fullscreen mode `Alt+Enter` uses (`native`: macOS only). |
| `SPEEDO_PRESENT_MODE=fifo\|fifo_relaxed\|mailbox\|immediate`, `SPEEDO_SWAPCHAIN_IMAGES=<count>` | The swapchain's presentation at startup, if the display offers it. |
| `SPEEDO_SHOW_STATISTICS=1`, `SPEEDO_SHOW_LIGHTS=1` | Open the Statistics or Lights window at startup. |
| `SPEEDO_OPEN_MENUS=View/Presentation` | Hold these menus open, for screenshots. |
| `SPEEDO_BROWSE=<dir>`, `SPEEDO_BROWSE_SELECT`, `SPEEDO_BROWSE_KINDS`, `SPEEDO_BROWSE_RECURSIVE`, `SPEEDO_BROWSE_OPEN`, `SPEEDO_BROWSE_ENVIRONMENT` | Open the file browser on a folder, with a selection, filters and an open action (see CLAUDE.md). |
| `SPEEDO_SHADER_TIERS=0` | Draw every material with the generic shaders rather than ones specialized for its layers. |
| `SPEEDO_GPU_TIMINGS=1` | Print each render pass's median GPU time at exit (debug and profile builds). |
| `SPEEDO_VALIDATE_SYNC=1` | Enable Vulkan synchronization validation (debug builds). |
| `SPEEDO_VALIDATION_BREAK=error\|warning` | Break into the debugger on validation messages of that severity (debug builds). |

## Profiling

Debug and profile builds are instrumented with [Tracy](https://github.com/wolfpld/tracy). Start the Tracy profiler from
`install/<triplet>/tools/tracy/` or from its VS Code launch configuration, then run the client. The CPU side shows the
main thread and the task threads (named `TaskThread <n>`), and the GPU side a context per queue with a zone for each render
pass and upload. Those GPU zones and the Statistics window's pass timings come from the same timestamps (one GPU
profiler per queue, `rhi::GpuProfiler`), so the two agree. The Statistics window (View > Statistics...) also shows
submits and the frame graph's memory, and in debug builds counts of live GPU objects.

## Repository layout

| Path | Contents |
|------|----------|
| `src/core` | Foundations: the task executor, futures, memory pools, file and path helpers, the application base class. |
| `src/platform` | The window system: windows, the windowed application base (its main loop's work, exit wake-ups), imgui's platform side. |
| `src/rhi` | The graphics abstraction over Vulkan: devices, queues, command buffers, buffers, images, pipelines, the swapchain. |
| `src/gfx` | Everything above the rhi: importers, models, textures, environments, cameras and views, the frame graph and renderer, the shaders (`src/gfx/shaders`, Slang), the scene, and the imgui layer. |
| `src/client`, `src/server` | The two programs: the client's windowed application (frame loop, user interface, loading) and its connection to the server. |
| `src/tools` | `assettest` and `filebench`. |
| `scripts/` | Setup, test and asset scripts, CMake toolchains and vcpkg triplets. Scripts are PowerShell, run with `pwsh` on every platform. |
| `ports/` | vcpkg overlay ports: patched versions of dependencies. |
| `resources/` | Fonts and other runtime resources; test assets are downloaded here. |
| `TODO.md` | What's planned, in progress and done. |
| `CLAUDE.md` | Detailed engineering notes: the architecture's rules, conventions, and the causes of past build and runtime failures. Read it before changing the rhi, the build or the toolchain. |
