# speedo

[![CI](https://github.com/djohansson/speedo/actions/workflows/ci.yml/badge.svg)](https://github.com/djohansson/speedo/actions/workflows/ci.yml)

![A glass dragon against a sunset, the sun's anamorphic streak across it and the cloth behind it seen through its body](docs/screenshots/dragonattenuation.jpg)

speedo is a real-time renderer built on Vulkan 1.4 for physically based images at interactive frame rates.

Light bounces through the scene from irradiance probes over a voxel clipmap, sharpened by a surface cache and a ray traced
near field, and every emissive surface lights its surroundings as an area light. Hardware ray queries trace soft shadows
from lights with a real size, glossy reflections, refraction through glass, and caustics; where a GPU has no ray tracing
hardware (and on macOS by default), BVHs on the GPU take their place. The sky is Earth's atmosphere, with the sun, moon
and stars where the date and time of day put them, aerial perspective and fog.

You see it all through a camera modeled on a real one: a physical sensor and exposure, lenses designed element by element
with their aberrations, distortion, ghosts and glare (anamorphic ones too), depth of field, motion blur and film grain,
and temporal upscaling by a port of AMD's FSR 3.1. Underneath are a visibility buffer, GPU culling with two-phase
occlusion, clustered lighting, order independent transparency, transmission and subsurface scattering, all scheduled by a
frame graph on a task based engine.

It runs on macOS (Apple silicon, through KosmicKrisp, mesa's Vulkan driver over Metal), Windows and Linux, built with the
same LLVM toolchain on all three. It can also run without a display or a GPU, which is how CI tests it.

<!-- one row, the same height: each image's width in proportion to its aspect ratio (3.25, 1.62, 2.15) -->
<p align="center">
  <a href="docs/screenshots/abeautifulgame.jpg"><img src="docs/screenshots/abeautifulgame.jpg" width="45%" alt="A golden chess piece in focus on a green marble board, the other pieces blurred behind it, through a 2x anamorphic lens"></a>
  <a href="docs/screenshots/sponza.jpg"><img src="docs/screenshots/sponza.jpg" width="22.5%" alt="Sponza's sunlit gallery through a 16 mm lens: a column, the arcade and its curtains, the sun's starburst and ghosts"></a>
  <a href="docs/screenshots/carconcept.jpg"><img src="docs/screenshots/carconcept.jpg" width="30%" alt="Three CarConcept variants side by side on the ground plane at sunset, through a 1.33x anamorphic lens"></a>
</p>

*The Khronos glTF sample models DragonAttenuation (at the top), ABeautifulGame, Sponza and CarConcept (click one for
its full size), under the physical sky at their times of day, with indirect lighting and ray traced shadows, reflections
and refractions.*

<!-- made with the profile client from View > Camera > Copy Pose's variables, SPEEDO_YEAR=2026 SPEEDO_GI=1, plus
SPEEDO_WINDOW_SIZE=3022x1898 SPEEDO_UI=0 SPEEDO_AUTOLOAD_EXIT=1200 SPEEDO_CAPTURE=<file.bmp>; the letterbox cropped, and
scaled to 1600 wide. Models under test-assets/ (resources/): glTF-Sample-Assets/Models/<model>/...
(ABeautifulGame/glTF-Binary-KTX-ETC1S-Draco, DragonAttenuation/glTF-Binary, Sponza/glTF, CarConcept/GLB).
ABeautifulGame: SPEEDO_CAMERA=0.0343,0.1526,0.2572,10.550,-3.884 SPEEDO_TIME_OF_DAY=10.500 SPEEDO_LENS=anamorphic-50
DragonAttenuation: SPEEDO_CAMERA=1.3303,1.0941,2.5190,3.171,-30.950 SPEEDO_AUTOLOAD_VARIANT=0 SPEEDO_TIME_OF_DAY=19.720
SPEEDO_LENS=anamorphic-50 SPEEDO_FOCUS=wide
Sponza: SPEEDO_CAMERA=6.1378,4.2769,-2.4115,-18.428,-104.616 SPEEDO_TIME_OF_DAY=14.440 SPEEDO_LENS=ultrawide-16
SPEEDO_FOCUS=wide
CarConcept: SPEEDO_CAMERA=2.5407,0.6069,0.7475,9.462,-58.063 SPEEDO_AUTOLOAD_VARIANTS=0,1,2 SPEEDO_TIME_OF_DAY=17.230
SPEEDO_LENS=anamorphic-50-133 SPEEDO_FOCUS=wide -->

## Features

- **Assets:** glTF 2.0 with every `KHR_materials_*` extension the Khronos samples use, material variants, Draco and
  meshopt meshes, KTX2/Basis and WebP textures, skins, morph targets and `KHR_animation_pointer`; Wavefront OBJ; zip
  archives of models; HDR panoramas; a ground plane, the scene's unit. [assets](docs/notes/assets.md)
- **Indirect lighting:** irradiance probes over a voxel clipmap, a surface cache and a ray traced near field, and emissive
  surfaces lighting the scene as area lights. [indirect lighting](docs/notes/indirect-lighting.md)
- **Ray tracing,** through hardware ray queries where the device has them (else BVHs on the GPU): soft shadows from lights
  with a size, glossy reflections, refractions through glass, and caustics. On macOS the BVHs are the default and ray
  queries opt in (`SPEEDO_RAY_QUERY=1`) until Apple fixes a GPU restart in Metal's ray tracing.
  [rendering](docs/notes/rendering.md)
- **Rendering:** a frame graph, a visibility buffer, GPU culling with two-phase occlusion, clustered lighting, a cached
  shadow atlas, order independent transparency, transmission and subsurface scattering, reversed depth, and one to four
  views of the scene in split screen. [rendering](docs/notes/rendering.md)
- **The camera:** a physical sensor and exposure (auto exposure, by the eye at night), lenses with their vignetting,
  aberration, distortion and glare (anamorphic ones too), depth of field, motion blur and grain, and temporal upscaling by
  a port of AMD's FSR 3.1. [rendering](docs/notes/rendering.md)
- **The sky:** Earth's atmosphere, the sun, moon and stars where the time of day and the date put them, aerial perspective
  and fog. [sky](docs/notes/sky.md)
- **The engine:** a task executor, Vulkan 1.4 behind a backend-neutral abstraction, crash reports with GPU breadcrumbs,
  and a headless mode (no window, surface or swapchain) for runs without a display. [core](docs/notes/core.md),
  [layering](docs/notes/layering.md), [rhi](docs/notes/rhi.md)
- **Testing:** every importer checked against reference decodes, the client run over the test sets with Vulkan
  validation, and its frames checked against the renderer's own references and a known answer, repeatably, with CI
  running all of it on every push. [visual testing](docs/visual-testing-plan.md), [assets](docs/notes/assets.md)

## Installing

Each release on GitHub has an installer for every platform: a `.dmg` for macOS on Apple silicon, an `.msi` for Windows x64,
and an AppImage for Linux x64 (glibc 2.43 or newer, as on Ubuntu 26.04), each with its sha256. They aren't signed by a
developer yet, so the system asks the first time: on macOS, System Settings > Privacy & Security > Open Anyway (or
`xattr -dr com.apple.quarantine /Applications/Speedo.app`); on Windows, SmartScreen's More info > Run anyway. The first
launch offers to download the test models while the driver creates its pipelines. An installed client's shaders come
precompiled: the installers ship no shader compiler. On macOS, the app brings its own Vulkan driver (KosmicKrisp, which
runs Vulkan on Metal), so there is nothing else to install.

## Using the client

- **File > Open...** (and Open Recent) is a file browser with thumbnails and previews of models, zip archives,
  panoramas and images. It opens one file or several, side by side or as authored.
- **Moving around:** drag with the left mouse button to look around, `W`/`A`/`S`/`D` to move, the scroll wheel for the
  move speed. `Alt+Enter` toggles fullscreen.
- **The interface fades away** when it isn't in use: move the mouse to the top of the window to bring the menu bar back.
- **View** holds the settings: the layout of views, presentation, the environment and time of day, the camera and its
  lens, the lighting features (each can be turned off to compare), debug views, and the Lights and Statistics windows.
- **View > Camera > Copy Pose** (`Ctrl/Cmd+Shift+C`) copies the camera and every changed setting as environment
  variables, which start the client in the same state, or Paste Pose applies to a running one: the way to report what
  you see.

## Getting started with development

To run speedo, an installer above is all you need; this is for building it from source.

### 1. Set up the machine

Run the bootstrap script for your platform from the repository root:

| Platform | Command |
|----------|---------|
| macOS    | `./setup_osx.sh` |
| Linux    | `./setup_linux.sh` |
| Windows  | `setup_windows.bat` |

The bootstrap script installs PowerShell 7.5+ if it's missing, then hands over to `setup.ps1`. It installs the system
compilers and SDKs (the Xcode Command Line Tools on macOS, the C++ build tools and the Windows SDK on Windows, the
distribution's build packages on Linux) and the other system packages (Homebrew, apt or winget), installs the pinned
release of vcpkg into `~/.vcpkg-tool`, and generates `CMakeUserPresets.json` and the VS Code launch and task
configurations. Re-run it after OS or SDK updates. The tools that only building vcpkg's packages from source takes (on
Linux the distribution's clang, meson's and autotools' tools) it installs only when the binary cache below lacks one of the
packages.

speedo builds with its own LLVM toolchain on every platform: clang, lld and libc++ from vcpkg's LLVM, one version
everywhere, with the system tools providing the platform's SDKs and libraries. Setup downloads the toolchain prebuilt,
along with every other dependency, from the project's shared binary cache, a public bucket read over https: for macOS 27
on Apple silicon, Windows x64, and Linux x64 on Ubuntu 26.04. A package the cache doesn't hold yet is built locally. The
dependencies include the Vulkan drivers built from mesa (the djohansson/mesa fork): KosmicKrisp on macOS, lavapipe on
Windows and Linux. See [build](docs/notes/build.md) ("vcpkg's shared binary cache").

### 2. Configure and build

Every build directory comes from a preset named `<triplet>-<config>`, e.g. `arm64-osx-clang-profile`,
`x64-windows-clang-profile` or `x64-linux-clang-profile`:

| Config    | Use it for |
|-----------|------------|
| `debug`   | Debugging. Vulkan validation and asserts are on, and it's slow. |
| `profile` | Optimized, with profiling (Tracy and GPU pass timings) and asserts. The best default for testing. |
| `release` | Optimized, without profiling (no Tracy): what the installers ship. |

```sh
cmake --preset arm64-osx-clang-profile && cmake --build build/arm64-osx-clang-profile
```

Always configure with `cmake --preset <name>` before building from a shell: the presets carry the toolchain's
environment, which the dependencies' hashes depend on. A configure without it, from a raw `cmake` command or a build
that reconfigures by itself, stops with a message telling you to run the preset. In VS Code, the CMake Tools
extension picks up the presets, and the generated launch configurations start the client, the server or the Tracy
profiler with the right environment.

The presets build with FASTBuild, through a launcher that caches every compiled object, locally (`~/.fastbuild/cache`)
and in the same shared bucket, so a build takes what CI or another machine of the platform compiled
(`SPEEDO_FASTBUILD_CACHE=0` builds without it). The code is built for a fixed CPU baseline (Apple's M1, x86-64-v3), never
the building machine's, so what one machine builds runs on the others. See [build](docs/notes/build.md) ("FASTBuild's
cache").

### 3. Run the client

The build puts `client`, the viewer, in the preset's build directory. Run it from the repository root, where it finds
`resources/`:

```sh
./build/arm64-osx-clang-profile/client
```

| Client option | Default |
|--------|---------|
| `-r <dir>`: the resource directory (fonts, shaders, test assets) | `resources/` |
| `-u <dir>`: the user profile directory (settings, caches, logs) | `.speedo/` |

From a checkout the shaders compile from their sources at startup, through a shader compiler library the client loads
only for that; an installed client gets them precompiled, and its package ships neither the compiler nor slang. A first
launch compiles them in the background (from a checkout) and has the driver create the pipelines, with the progress
shown in a Loading window while the interface stays responsive; later launches take them from the profile's caches and
start in seconds.

On macOS, point the Vulkan loader at KosmicKrisp, which setup restores from the binary cache (with a workaround for a GPU
restart in Apple's ray tracing, see [indirect lighting](docs/notes/indirect-lighting.md)):

```sh
export VK_DRIVER_FILES=$(ls $PWD/install/*/share/vulkan/icd.d/kosmickrisp_mesa_icd.*.json | head -1)
```

On Windows and Linux, the client uses the system's GPU driver unless told otherwise. To render without a GPU, for
reference renders or on a machine without one, choose lavapipe explicitly with the same variable (setup installs it
there; it is slow), e.g. `VK_DRIVER_FILES=$PWD/install/x64-linux-release/share/vulkan/icd.d/lvp_icd.x86_64.json`.

For a debug build from a shell, also set `VK_LAYER_PATH=$PWD/install/<triplet>/share/vulkan/explicit_layer.d` so the
validation layers are found. The VS Code launch configurations set the layer path, and on macOS the KosmicKrisp driver.

`SPEEDO_HEADLESS=1` runs the client without a window: no display is needed, and it draws into an image of
`SPEEDO_WINDOW_SIZE` instead of a swapchain, which `SPEEDO_CAPTURE` saves. See [layering](docs/notes/layering.md).

### Applications

The build makes these programs, all in the preset's build directory and run from the repository root as the client is:

| Program     | What it does |
|-------------|--------------|
| `client`    | The viewer: a window with a menu bar, one to four views of the scene, and tools for inspecting it, in a translucent, frosted interface styled after macOS's. |
| `server`    | A small companion process the client talks to over ZeroMQ: the start of a distributed task system. |
| `assettest` | Imports every model, image and environment it's given and checks the results, and the BVHs against brute force and the lens prescriptions: the main automated test. |
| `imagecompare` | Compares a capture of the client's with a reference: the pixels past thresholds, the luminance difference sharp and blurred, the light gained or lost. What the visual checks pass or fail by. |
| `convergence` | How long a still view's lighting takes to settle, from a series of captures (`scripts/perfbaseline.ps1`). |
| `filebench` | A benchmark of file reading, the importers (`--import <files>`) and zip extraction (`--zip <archives>`). |
| `taskbench` | A benchmark of the task system's overheads and how full its task pool gets. |
| `shaderprecompile` | Compiles every shader variant the client may load into the precompiled sets an installer ships (run by `scripts/package.ps1`). |

## Testing

The test sets are downloaded from their sources (Morgan McGuire's Computer Graphics Archive, and the Khronos
glTF-Sample-Assets and glTF-Sample-Environments, about 4.5 GB), pinned and checked against checksums, into
`resources/test-assets/` (or `$SPEEDO_TEST_ASSETS`). `scripts/assettest.ps1` runs `assettest` over them, and with
`-Client` opens each model in the client, failing on load errors, asserts and validation messages:

```sh
pwsh -Command '$p = & scripts/fetch-test-assets.ps1 -Quick; & scripts/assettest.ps1 -Preset arm64-osx-clang-profile -Client -Validate -Work <work dir> @p'
```

`-Quick` is a model or two per feature, about 60 client runs in a few minutes; without it, the full sets. Reuse the work
directory between runs: it holds the extracted archives and the caches. The client windows stay behind your others
without taking the focus; `-Headless` runs them without windows, for a locked screen or a machine without a display.
See [assets](docs/notes/assets.md) for the checks and the hand-made test models.

`scripts/visualtest.ps1 -Preset <preset>` checks the frames themselves. Each check renders a scene twice, headless and
repeatable (`SPEEDO_REPEATABLE=1`: the same image from run to run as far as the driver allows), and `imagecompare`
measures the pair against thresholds: the traced shadows and the shadow atlas against their references, the emissive
surfaces' light against theirs, temporal upscaling against none, the light inside a furnace (a closed box of emitting,
reflecting walls) against its known answer, and, on lavapipe, hardware ray queries against the BVHs. See
[visual testing](docs/visual-testing-plan.md).

CI runs on every push to `master` and on pull requests, on GitHub's hosted runners for macOS, Windows and Linux, which
have no GPU. Each job sets up from the shared caches, builds the debug, profile and release configurations, and runs the
importer checks; on Windows and Linux it then runs the client headless on lavapipe, validated, and the visual checks. A
push writes what the runners built to the shared caches, and once every job has passed, prunes them. The GPU drivers
(KosmicKrisp, the desktop ones) are tested on developers' machines. See [build](docs/notes/build.md) ("CI").

Environment variables start the client in a known state, for bugs, measurements and screenshots, e.g.
`SPEEDO_AUTOLOAD_MODEL`, `SPEEDO_AUTOLOAD_ENVIRONMENT`, `SPEEDO_CAMERA=x,y,z,pitch,yaw`, `SPEEDO_AUTOLOAD_EXIT=<frames>`,
`SPEEDO_CAPTURE=<file.bmp>`, `SPEEDO_BACKGROUND=1`, `SPEEDO_HEADLESS=1` and `SPEEDO_GPU_TIMINGS=1`. Copy Pose writes them
for you; the notes describe each where its feature is.

## Profiling

Debug and profile builds are instrumented with [Tracy](https://github.com/wolfpld/tracy) (release builds, which the
installers ship, have none of it): start the profiler from `install/<triplet>/tools/tracy/` or its VS Code launch
configuration, then the client. The GPU side has a zone per render pass, from the same timestamps as View > Statistics.
[rendering](docs/notes/rendering.md) ("GPU timings")

## Releasing

`scripts/release.ps1` makes a release from a clean `master`: `-Bump patch` (the default), `-Bump minor` or `-Bump major`
raises `src/version.h`'s version (or `-Version <x.y.z>` sets it), commits it and tags the commit `v<version>`. With `-Push`
it pushes `master` and the tag; without, it prints the push command. The tag starts the release workflow, which checks it
against `src/version.h`, builds and packages the release preset on each platform's CI runner, and attaches the installers
and their checksums to a draft release, published by hand once checked. Running the workflow by hand (Actions > Release)
packages without releasing, the installers kept as the run's artifacts. `scripts/package.ps1` makes this platform's
installer from a local release build. See [build](docs/notes/build.md) ("Releases").

## Documentation

| Document | Contents |
|----------|----------|
| [CLAUDE.md](CLAUDE.md) | The rules most changes need: the layering, conventions, and the workflow. |
| [docs/notes/](docs/notes/) | Each subsystem in depth: [core](docs/notes/core.md), [layering](docs/notes/layering.md), [rhi](docs/notes/rhi.md), [rendering](docs/notes/rendering.md), [indirect lighting](docs/notes/indirect-lighting.md), [sky](docs/notes/sky.md), [assets](docs/notes/assets.md), [build](docs/notes/build.md). |
| [docs/visual-testing-plan.md](docs/visual-testing-plan.md) | Checking what the client draws: repeatable captures, `imagecompare`, the visual checks, and the steps still to come. |
| [docs/performance-plan.md](docs/performance-plan.md) | Frame rate and convergence in the heavier scenes: the benchmark poses and baselines. |
| [TODO.md](TODO.md) | What's planned, in progress and done. |

## Repository layout

| Path | Contents |
|------|----------|
| `src/core` | The task executor, memory pools, files, the application base. |
| `src/platform` | The window system (headless too) and imgui's platform side. |
| `src/rhi` | The graphics abstraction over Vulkan. |
| `src/gfx` | Everything above the rhi: importers, the scene, the renderer and frame graph, the shaders (Slang). |
| `src/client`, `src/server` | The two programs. |
| `src/tools` | `assettest`, `imagecompare`, `shaderprecompile`, the benchmarks (`filebench`, `taskbench`, `convergence`), and macOS's Metal repros (`src/tools/macos`). |
| `scripts/` | Setup, test (`assettest.ps1`, `visualtest.ps1`), asset, cache, packaging and release scripts (PowerShell), CMake toolchains and vcpkg triplets. |
| `.github/workflows/` | CI on every push, and the release workflow on `v*` tags. |
| `ports/` | vcpkg overlay ports: patched dependencies, mesa's Vulkan drivers (KosmicKrisp, lavapipe), and FASTBuild's cache plugin and launcher. |
| `resources/` | Fonts and hand-made test models; test assets are downloaded here. |
