# Asset import and testing

How models, images and environments get into the scene, and how the importers are tested. glTF and OBJ models and their images are imported (`gfx/meshimport.h` dispatches on the extension), cached under a key that includes each importer's version, and uploaded through rhi's staging buffers. A model can bring its own viewing conditions: an exposure, a ground plane, a unit for its coordinates, and material variants chosen as it opens. The test sets are fetched from their sources by manifest, and `assettest` checks the importers, the BVH and the lenses without frames and, with `-Client`, runs the client on every model.

Speedo decodes models, images and environment panoramas on the CPU in `gfx`, caches the result and uploads it through staging buffers. Two model importers (glTF and OBJ) produce one `gfx::mesh::Mesh`; glTF carries most of the renderer's material features (the `KHR_materials_*` extensions, instancing, skins, morph targets, animation, cameras, lights). Rendering is linear in a half float target, exposed (manually or automatically) and tonemapped at the end. Files come from the file browser, archives and sets, or the `SPEEDO_AUTOLOAD_*` variables. The test sets are downloaded from their public sources and checked by the `assettest` tool and script, with the client loading each model as well. `docs/environment-variables.md` describes the variables named here.

## Contents

- [Importers and loading](#importers-and-loading)
- [Caches](#caches)
- [glTF](#gltf)
- [OBJ](#obj)
- [Images](#images)
- [Environments](#environments)
- [A model's own conditions](#a-models-own-conditions)
- [Exposure and tonemapping](#exposure-and-tonemapping)
- [The ground plane](#the-ground-plane)
- [The scene's unit](#the-scenes-unit)
- [A file's material variants, chosen as it opens](#a-files-material-variants-chosen-as-it-opens)
- [Archives of several models](#archives-of-several-models)
- [Test sets](#test-sets)
- [assettest](#assettest)

## Importers and loading

- Importers: `gfx::obj::Import` (tinyobjloader), `gfx::gltf::Import` (cgltf), `gfx::image::Import` (stb_image, stb_image_resize2, stb_dxt, libwebp, libktx for KTX2, which also transcodes Basis Universal). `gfx/meshimport.h` dispatches on the extension (`mesh::Import`, `IsModelFile`, `Dependencies`).
- `gfx::Model::Load` and `gfx::LoadTexture` cache the result and fill rhi staging buffers (`Buffer::CreateStaging`, before taking a queue's lock). A failed load prints why and returns null instead of trapping.
- The loaders don't wait for their uploads: they return the resource with its `gfx::Upload` (transfer timeline semaphore and value, queue family). Whatever uses it on another queue must wait for the upload and acquire the resource for its family (`TransitionThenBind` does both); resources are exclusive-sharing and released to the graphics family. Staged images stay in `kTransferDestination`, since transitioning from `kUndefined` would let the driver discard the contents.
- `InstallModel` switches model, textures and materials in one draw thread step, then sets the views to the model's first camera or frames its bounds (`Views::SetScene`, `Views::FrameBounds`).

## Caches

- The model cache key includes the importer's tag in the loader's params hash (`src/gfx/model.cpp`): `objimport-vN` with tinyobjloader's version, `gltfimport-vN` with the versions of cgltf, draco and meshoptimizer. Images (`src/gfx/texture.cpp`) have `imageimport-vN` with stb's version, and libwebp's or libktx's for those formats; environments `environment-vN` and `cache-vN`. When an importer changes what it produces, bump its tag, or stale caches keep the old output.
- A glTF scene other than the default and each `KHR_materials_variants` variant are cache entries of their own. Assets outside `RootPath` are cached under `<user profile>/external/<absolute path>`. Nothing of the ground plane is cached.

## glTF

- A scene (`mesh::ImportOptions::scene`, else the default or the first; View > Scene or `SPEEDO_AUTOLOAD_SCENE=<index>`) is flattened into one mesh with node transforms applied. A mirroring transform reverses the winding and must flip the cofactor normal matrix back (`NegativeScaleTest`). Texcoords already have v = 0 at the top; normal maps share the OBJ convention.
- `EXT_mesh_gpu_instancing` is drawn instanced: the node's primitives once, a transform per instance in `Mesh::instances`, each submesh naming a range. Mirroring instances are sorted last and drawn with clockwise front faces (`CommandEncoder::SetFrontFace`).
- Points and lines are submeshes of their own (`ModelSubmesh::topology`, a pipeline per topology via `Pipeline::BindPipelineAuto(cmd, variant)`), drawn unlit without normals. Double sided materials are drawn with culling off (`CommandEncoder::SetCullMode`) and the shader flips back face normals.
- Alpha: `mesh::Material::alphaCutoff` (0 for OPAQUE and BLEND). BLEND is exact order independent transparency by per pixel linked lists (`FragmentTransparent`, `OitNode`), drawn after the opaque submeshes; `ComputeMain` sorts the 16 nearest layers per pixel and blends them. The node pool is sized with the render target and fragments past it are dropped (a policy, not a capacity).
- Textures: each is a `TextureRef` (`gfx/textureref.h`: path, texcoord set, `KHR_texture_transform`, `rhi::SamplerDesc`) applied in the shader through `gTextureViews`. A model's distinct samplers use the slots besides the default's and the clamping one (`kModelSamplerSlots`). Images embedded in buffers or data uris are named by model file and index (`TextureRef::embeddedImage`) and decoded from memory.
- Usages choose the format: `kColor` (srgb), `kMask`, `kOcclusion` and `kAlpha` (BC4), `kNormal` and `kBump` (BC5), `kMetallicRoughness` (BC5: roughness in r, metallic in g), `kLinear`. Occlusion is imported as the red channel (glTF packs occlusion, roughness and metallic together) and darkens only the ambient term.
- Compressed content: meshopt (`EXT_meshopt_compression`, `KHR_meshopt_compression`) and draco are decoded at import. Textures use their plain image, else `KHR_texture_basisu` (KTX2) or `EXT_texture_webp`. KTX2 textures with a full mip chain keep their own blocks where possible (`ImportKtx2`: Basis to BC7, or BC1, BC3, BC7 as is; `Image::ownBlocks`); other KTX2 and WebP images are decoded to rgba8 and compressed like any other.
- Materials: `Shade` is the glTF metallic-roughness BRDF (GGX, height correlated Smith, Schlick) over `gLights` plus image based lighting. Every `KHR_materials_*` of the glTF-Sample-Assets is drawn: unlit, emissive_strength, ior, specular, sheen, clearcoat, transmission, volume, dispersion, anisotropy, iridescence, diffuse_transmission, retroreflection, volume_scatter and variants; specular-glossiness is supported. A primitive without a material gets the spec's default.
- Transmission draws in two phases when a model has transmissive materials (`MainPassPhase`): the opaque submeshes, a Transmission pass that copies the color to a mipped texture, then the transmissive and blended ones, which sample it along the refracted ray. Volume scatter is screen space subsurface scattering in its own passes (`BuildScatterDrawList`).
- Emissive (`emissiveFactor` times `KHR_materials_emissive_strength` and the srgb texture) is added after the lighting, unclamped. A sampled emissive texel is taken less a black level (`EmissiveTexel`, `kEmissiveBlack`), as an 8 bit texture's black isn't 0 once compressed.
- Tangents come from the file, else MikkTSpace for triangles with a normal map, else the fragment shader derives the frame from screen space derivatives.
- Materials live in `gMaterialData` from index 1, starting at `SHADER_TYPES_MATERIAL_COUNT` and growing to the next power of two (`GrowMaterials`).
- Lights are `KHR_lights_punctual` (`ModelDesc::lights`). Cameras are imported (`SceneCamera`, `ModelDesc::cameras`; none for a set): the views use the first and View > Cameras... picks another.
- Animation (`SceneAnimationData`, `gfx/sceneanimation.h`): node animations, skins and morph targets are drawn moving. Animated nodes keep meshes in node space with instances linked to the node; skinned meshes keep vertices in mesh space, blended by `gJointMatrices`; morph deltas are added before skinning. `Model::Animate` writes the per frame buffers on the draw thread, and `AnimatedBounds` grows the bounds by the animations' poses. `KHR_node_visibility` hides nodes. `KHR_animation_pointer` is read from the json by the importer (cgltf skips it): node pointers become ordinary channels, the rest (materials, texture transforms, lights, cameras, visibility) are applied after `Model::Animate` (`ApplyPointerValues`).
- View > Animation picks the animation (the first plays on load). `SPEEDO_ANIMATION_TIME=<seconds>` freezes it and `SPEEDO_ANIMATION=<index>` or `none` picks which; Copy Scene writes the time and Paste Scene holds it.

## OBJ

- OBJ defines no up axis or handedness, so the importer follows the renderer's conventions: counter-clockwise front faces and `v` flipped so `v = 0` is the first image row. Z up scenes load as they are.
- Runs of faces whose winding disagrees with the file's normals are flipped. Missing normals are generated, smoothed within a smoothing group (60 degrees for group 0); all of a mesh's normals are generated where they don't follow its faces (`stats.normalAlignment` below `kMinNormalAlignment`).
- The importer triangulates by halving (`ForEachFaceTriangle`), not tinyobj: triangles and quads come out as a fan would, and large faces avoid a fan's slivers, which no BVH separates.
- Materials: diffuse, alpha (`map_d`) and normal (`norm`, else `map_bump`/`bump`) textures. Bump textures are usually height maps but some are normal maps; the importer tells them apart by color (bluish) and converts heights to normals (scaled by `-bm`). Faces without a material use material 0, which opening an image textures.
- Materials are dielectrics: `Ks` scales the specular, `Ns` gives roughness (`sqrt(2 / (Ns + 2))`). Emission is `Ke` and `map_Ke` (a `map_Ke` with `Ke 0` is the map alone).
- Glass is an `illum` of 4, 6, 7 or 9 with a transmission filter `Tf` that is neither all ones nor all zeros (exporters write `Tf 1 1 1` everywhere): transmission by the filter's largest component, tinted by Kd, thin walled, with the ior `Ni` where above 1.
- Faces whose material has a normal or bump map get MikkTSpace tangents (`mesh::GenerateTangents`, `gfx/tangents.h`).

## Images

- `gfx::image::Import` decodes (the byte overload tells formats apart by signature), resizes and compresses. Color textures are srgb with mips filtered in linear space.
- Opening an image puts it on material 0 (`SPEEDO_AUTOLOAD_IMAGE`).

## Environments

- `gfx/environment.h` (cpu only): an equirectangular panorama (+y up, +x at its center: Blender's convention), a Radiance `.hdr` (`environment::Import`), a uniform one (`uniform:<r>,<g>,<b>`; `uniform:0,0,0` is none), or by default and in automated runs the sky (docs/notes/sky.md). View > Environment picks one, and `SPEEDO_AUTOLOAD_ENVIRONMENT` loads one. A model with a directional light of its own replaces the sky unless `SPEEDO_SKY_WITH_MODEL_LIGHTS=1`.
- A panorama's strongest compact sources are its dominant lights (`environment::ExtractDominantLights`, at most `kMaxDominantLights`). They light the scene as directional lights and cast shadows; one within 5 degrees of a model's own directional light is merged with it (`SPEEDO_MERGE_LIGHTS=0` turns that off).
- `LoadEnvironment` decodes and extracts on the cpu (cached); `InstallEnvironment` records the prefilter on the gpu (`gfx::EnvironmentFilter`, `environment.slang`): 6 levels of GGX prefiltered radiance, 6 of the sheen's Charlie lobe, and 9 spherical harmonics coefficients (`EnvironmentData`). Intensity and rotation are `SPEEDO_ENVIRONMENT_INTENSITY` and `SPEEDO_ENVIRONMENT_ROTATION`. The panorama is also the backdrop where nothing was drawn.
- `assettest` runs the same kernels on the cpu (slang compiles `environment.slang` to C++ at build time; `src/tools/environmentkernels*`), so they can't use shared memory or group barriers. It checks the prefiltered levels' and irradiance's means, the dominant lights, and the sky's sun.

## A model's own conditions

The glTF research set's scenes come with a `conversion.yaml` whose `reference_render` block says how their path traced references were made, and the client opens such a model in those conditions (`windowedapplication::ReadModelConditions`). It reads `environment_map`, `environment_strength` and `exposure_multiplier` (a missing environment key means none) and sets what a pose's `SPEEDO_*` variables name: the environment, a manual exposure plus the multiplier's stops, PBR Neutral, no ground plane, and transmission without a volume as solids (`SPEEDO_SOLID_TRANSMISSION`).

The autoload and a pasted pose take them under their own variables; File > Open..., Open Recent and the browser apply them as the model opens (`ApplyModelConditions`), skipping what the process's variables set (an empty variable counts as unset). They are the model's alone: the next model opened has them put back first (`UndoModelConditions`): each setting that still holds their value returns to its earlier one, and the environment to the previous one or the physical sky. See docs/research-scenes-plan.md.

## Exposure and tonemapping

- `ComputeMain` scales the `R16G16B16A16_SFLOAT` target by the exposure (View > Cameras... > Compensation, in stops), tonemaps it (View > Tonemapper or `SPEEDO_TONEMAPPER`) and the srgb curve is applied when copying to the swapchain. Tonemappers (`TONEMAPPER_*` in `capi.h`, implemented in `tonemap.slang`; names in `ui::kTonemappers`, `SPEEDO_TONEMAPPER=pbr-neutral|aces|agx|reinhard|linear`):
  - Khronos PBR Neutral (the default) leaves colors up to about 0.76 nearly as they are, so base colors show as themselves under a white light, and compresses brighter ones toward white.
  - ACES (Hill's fit) is the filmic look: more contrast and saturation, bright colors shifting toward white.
  - AgX (Wrensch's fit, Blender's) is a filmic curve over a wide range of stops that desaturates bright colors smoothly toward white without their hues shifting.
  - Reinhard divides each color by one plus its luminance: simple, flat in the highlights.
  - Linear (clamped) applies no curve, so what is brighter than 1 clips: for judging the exposure itself.
- Auto exposure (View > Cameras... > Mode: Program, on by default; `SPEEDO_AUTO_EXPOSURE=0` for captures compared at a fixed exposure) needs no pass that computes it: the Exposure Clear pass empties the frame's slice of the histogram, `ComputeMain` counts pixels by log2 luminance before exposure into it (`gExposureHistogram`, `SHADER_TYPES_EXPOSURE_BINS`), the Exposure Readback pass (a `SideEffects` pass declaring the histogram, and the cull, indirect lighting and visibility statistics, `HostRead`) makes it readable by the host, and `UpdateAutoExposure` reads it after the frame's fence and averages the log luminance between the 50th and 95th percentile.
- Dim pixels count for less (below `kPhotopicLog2`, by their share of that level), so at night the lit parts govern. The target is `log2(0.2) - mean`, lowered as the mean falls from daylight to night and to moonlight (`kMoonlightStops`), so a night is darker than a day. The exposure follows by exponential decay in elapsed time, independent of frame rate. The Exposure slider is added on top; `SPEEDO_EXPOSURE_LOG=1` prints the numbers.
- Each split screen view (at most `SHADER_TYPES_SPLIT_VIEW_COUNT`) has its own histogram and exposure (`ViewData::exposure`).

## The ground plane

On by default (the file browser's "Ground plane", `SPEEDO_GROUND_PLANE=0` for none in the autoloads and Paste Scene; `ModelGround`, `Model::Load`'s `ground`). It gives shadows and caustics somewhere to land. `AddGround` in `model.cpp` adds it to the staged triangles after the model's cache entry is read or its set merged.

- Geometry: a patch around the model's center, 1.25 times its largest dimension to each side, just below its lowest point, facing up, single sided, of tiles of a round size aligned to the world origin; around it 10 rings, each 4 times as far out as the one inside, to a million times the patch, so the sky's air hides the edge and the views need no far plane.
- Look: light gray, checkered in squares of a meter (`MaterialData::checker`, `kGroundChecker`, filtered so it fades to its mean toward the horizon), roughness 0.8. It has two submeshes and materials of its own (`Model::GetGroundMaterial`, `GetHorizonMaterial`, not `ModelMaterial` fields, which the model cache serializes) and is hazed toward the backdrop's color at the horizon (`GroundHorizonHaze` in `fragment.slang`).
- The GI and shadows are the model's alone, which the ground takes: the rings cast nothing and are in no voxels, BVH or ray query structure (`Staged::untracedMaterial`, `untraced`); the patch casts nothing (`BuildShadowDrawList`) but takes the model's shadows, caustics and light. The indirect lighting's clipmap covers the model's bounds (`FrameInputs::casterBounds`).
- `ModelDesc::bounds` takes in the patch; `Model::GetFramedBounds` leaves the ground out, which the views frame and orbit. `Scene::SceneState::ground` says what the installed model stands on, which View > Scene and Variant keep.

## The scene's unit

`gfx::SceneUnit` (`gfx/units.h`) says what the file's coordinates are in: meters, centimeters, millimeters, inches, feet or a custom meters per unit (`Scene::Settings::metersPerUnit`, `PushConstants::metersPerUnit`).

- It affects what is physical: the lights' inverse square falloff in meters (`LightFalloff`; ranges stay in file units), the ground checker's squares of a meter, and the air (`AerialPerspectiveData::metersPerUnit`). What is relative to the model (framing, move speed, clipmap, cascades) doesn't.
- Resolved as the model installs (`ResolveSceneUnit` in `client/ui.cpp`, once per scene file), in order: the unit chosen for it (`ChooseSceneUnit`: the browser's "Units" dropdown, `SPEEDO_UNITS=<m|cm|mm|in|ft>` or `SPEEDO_METERS_PER_UNIT=<meters>`, or a pasted pose's), which is then remembered; else the remembered unit (`RememberedUnit`, `<user profile>/units.txt`); else a guess (`gfx::GuessUnit`).
- The guess: a glTF is in meters, as its spec says; an OBJ is centimeters if more than `kCentimeterExtent` (500) units across, else meters; a set is meters, as its files are scaled to one size. View > Units shows the unit and pins one for the file; Automatic forgets it.

## A file's material variants, chosen as it opens

- A glTF file with `KHR_materials_variants` opened by the user (browser, File > Open Recent or an archive's chooser: `LoadChoice::kAsk`) first asks which variants to open (the Variants popup; `mesh::VariantNames` reads the json alone). One choice opens the file with it; several lay it out to compare.
- The autoloads and Paste Scene never ask (`LoadChoice::kDefault`): `SPEEDO_AUTOLOAD_VARIANT=<index>` opens one, `SPEEDO_AUTOLOAD_VARIANTS=all` or a comma separated list of indices and `default` (`ui::ParseVariants`) lays several out. A file without variants is laid out once with its default materials (`ui::LayOutVariants`).
- A layout is copies of the files, each with its variant (`Scene::LoadVariants`, through `Model::Load`'s per-file `options`, each its own cache entry), merged as a set. The default arrangement is one behind the other (`ModelArrangement::kBehindEachOther`); side by side is the popup's choice or `SPEEDO_AUTOLOAD_ARRANGEMENT=side-by-side`. Copy Scene writes each copy's variant (`Model::GetLayout`).

## Archives of several models

- Zip archives (opened, or a `.zip` in `SPEEDO_AUTOLOAD_MODEL`) are extracted once into `<user profile>/archives/<name>-<hash of path, size and time>` with `core::zip` (`core/zip.h`: our own reader, libdeflate's inflate and crc-32), then loaded like other files. The extractor reads each entry by its local header, as some archives have stale central directories. The `.extracted` marker's length says which extractor made a directory; an older one's is redone (`ExtractArchive`). `assettest` extracts the same way.
- Archives of several models are sets of variants, not scenes: each file stands alone at the origin and the files needn't share a scale. `Model::Load(filePaths)` loads them as one model, side by side in a grid facing the camera or on a ground plane (`ModelGround::kPlane`), each scaled to one size and centered in its cell by its instance transforms, through its own cache entry, merged as staging data before one upload (`model::Merge`). What moves keeps moving (`MergeAnimation`). A set has no lights or cameras.
- Copy Scene writes a set's files for `SPEEDO_AUTOLOAD_MODEL` separated by `;`, with `SPEEDO_AUTOLOAD_ARRANGEMENT=as-authored` where so. Autoloading a zip or directory loads all its models as a set, as does the browser's Open with several selected.
- `assettest.ps1` runs the client once per archive and once per subdirectory with several models, not per file. glTF sample models are such subdirectories, of encodings of one model (glTF, glTF-Binary, Draco, KTX2, ...); unsupported encodings are skipped (`mesh::Unsupported`). `assettest` compares the encodings (equal counts, bounds within 1%) as warnings only.

## Test sets

- The sets come from their sources, not local copies. `scripts/fetch-test-assets.ps1` downloads them into `resources/test-assets` (gitignored; or `$SPEEDO_TEST_ASSETS`) and prints their paths. Each is a manifest in `resources/test-asset-manifests`, which the client's download uses too: `mcguire.txt` (Morgan McGuire's Computer Graphics Archive, OBJ), `gltf.txt` (Khronos glTF-Sample-Assets at a pinned commit), `environments.txt` (Khronos panoramas), `research.txt` (the glTF research scenes, a pinned release archive) and `research-files.txt` (two research scenes and their panorama, file by file).
- A manifest names the directory, the url, the zips to extract and each file's size and sha256, tab separated, path last. A file is downloaded unless it is there with its size and sha256; a mismatch is kept as `.unverified` and reported until the manifest is updated. `scripts/update-gltf-manifest.ps1` writes the glTF manifest from a checkout. An `extract <from> <into>` line unpacks the zips of a directory side by side into another (Bistro's parts, which reference each other).
- `fetch-test-assets.ps1` takes `-Dir`, a switch per set (`-McGuire`, `-Gltf`, `-Environments`, `-Research`; none fetches all), `-Quick` (fetches all, prints only the quick tier's paths) and `-Minimal` (fetches only what the quick tier names, as CI does, and never narrows what is there).
- The client downloads them too (`client/testassets.cpp`): a first launch offers the quick set or everything, File > Download Test Assets... offers them again. Automated runs are never offered; `SPEEDO_DOWNLOAD_TEST_ASSETS` tests it.
- Hand-made models fill what no download covers and are always among the printed paths: `resources/test-models/gltf` (sparse indices, instancing transforms, blend order, many samplers, many lights, caustics, a moving interior, ...) and `resources/test-models/obj` (`BumpDomes`). Some are made by scripts in `scripts/test-assets/`.
- The quick tier is `fetch-test-assets.ps1 -Quick`: the models `resources/test-asset-manifests/quick.txt` lists (a model or two per feature) plus the hand-made ones. The full sets run before a commit that touches many models.
- Known asset problems that only warn: OBJ normals that don't follow the faces (generated), normals that get replaced, and stray vertices (`assettest`: far from the median point). Dense overlapping triangle sets (the geodesic duals) give BVHs so deep that a ray stops after `BVH_MAX_STEP_COUNT` nodes with its nearest hit (`BvhHit::capped`; a capped shadow ray is shadowed); `CheckBvh` reports capped rays as a warning.

## assettest

`src/tools/assettest.cpp` imports every model and image the way the client does and checks the result. It takes files, directories and zip archives (searched recursively for models, images and `.hdr` panoramas), prints one line per asset (PASS, WARN, FAIL) and a summary, and exits 1 if any asset failed. `--models-only`, `--images-only` and `--jobs N` apply. The textures that materials name are checked too.

- Models: index ranges, normals against the triangles', missing textures, stray vertices, an up axis hint, tangents, culling chunks, animations (finite transforms), morph deltas, other glTF scenes, and encodings against each other.
- BVHs (`CheckBvh`): the static and the moving layouts, traversed on the cpu as `bvh.slang` does, against every triangle in double precision.
- Images: compression error (psnr over the channels the shader samples), mip chains, unwritten blocks. A file's own KTX2 blocks and mips only warn.
- Environments: the filter kernels, dominant lights and the sky's sun.

`scripts/assettest.ps1 <zips, dirs or panoramas>` runs the tool, and with `-Client` also loads each model in the client.

| Option | Meaning |
| --- | --- |
| `-Preset` | the build preset (default: the first configured release preset) |
| `-Client`, `-ClientOnly` | load each model in the client; the latter skips the assettest run |
| `-Validate` | validation in a profile or release preset's clients (the Vulkan layer, Metal's API validation, D3D12's debug layer); a run fails on its error and warning messages |
| `-Headless` | `SPEEDO_HEADLESS=1`: no display needed (not with `-Presentation`) |
| `-Presentation` | also runs View > Presentation's options and fullscreen switches, one at a time |
| `-GraphicsApi` | which apis to run (default: all of the platform's: vulkan and d3d12 on Windows, vulkan and metal on macOS, vulkan on Linux), each client once per api |
| `-Work`, `-Jobs` | the work dir (archives, profiles, logs); clients at once (default from the machine's cores and memory) |

- A client run exits `SPEEDO_AUTOLOAD_EXIT` frames (default 30) after its loads finish and is killed after `ASSETTEST_CLIENT_TIMEOUT` seconds (default 600). A client that doesn't end when killed (on macOS, stuck in the kernel after the gpu hung) stops the whole run as a wedged gpu: the others running are stopped, the rest counted as not run, and the run says to reboot, where it waited on that client forever before (`Stop-Client` in `scripts/gpu.ps1`, as `visualtest.ps1` and `researchcompare.ps1` do).
- On macOS the client runs of `assettest.ps1`, `visualtest.ps1` and `researchcompare.ps1` hold the gpu's lock (`temp/gpu.lock`, `Enter-GpuLock` in `scripts/gpu.ps1`): shared, so that such runs go side by side, and alone where their clients take hardware ray queries (`SPEEDO_RAY_QUERY=1`), whose ray tracing hangs or restarts the gpu with other gpu work beside it, so that either waits for the other whoever started it. A run that waits says what holds the lock. Windows and Linux take no lock and run side by side.
- A client run fails on load errors, failed asserts, validation messages and sanitizer reports. `SPEEDO_BACKGROUND=1` keeps its window behind others and `SPEEDO_PRESENT_MODE=fifo` is used for the runs.
- Extra runs: labels in `scripts/test-assets/splitscreen.txt` go again in split screen (`<label>-splitscreen`); those in `scripts/test-assets/norayqueries.txt` go again with `SPEEDO_RAY_QUERY=0` (`<label>-norayqueries`), so the shaders' variant without ray queries compiles and draws too. Every run sets `SPEEDO_AUTOLOAD_VARIANTS=all`.
- Parallel clients: each worker has a profile of its own (`<work>/user-<n>`), a model always goes to the same worker so its caches stay warm, and an idle worker takes work from the busiest. In a new work dir the first client of each api is a warm-up that compiles the shaders once, and worker profiles start as copies of its profile. Logs are kept per preset in `<work>/logs/<preset>`.
- Display and power: with a window the screen must be on and unlocked; `-Headless` only keeps the system awake, as an idle sleep makes the client's crash watch report a hang.
- Tiers: the quick tier (profile preset, `-Validate`, `-Headless`, every graphics api of the platform) runs before each commit. The full sets add a debug pass with `-ClientOnly` and the same `-Work`, which checks what only a debug client does (`ASSERT`s, rhi's object tracking).
- Typical use: `pwsh -Command '$p = & scripts/fetch-test-assets.ps1 -Quick; & scripts/assettest.ps1 -Preset <preset> -Client -Validate -Headless -Work <scratch>/quick @p'`.
