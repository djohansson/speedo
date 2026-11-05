# Asset import and testing

A note of CLAUDE.md (its index names each): the importers, the caches, the ground plane, the scene's unit, the test sets and the regression.

## Asset import and testing

Decoding is CPU only and lives in `gfx`: `gfx::obj::Import` (tinyobjloader), `gfx::gltf::Import` (cgltf) and
`gfx::image::Import` (stb_image, stb_image_resize2, stb_dxt). Both model importers produce a `gfx::mesh::Mesh`
(`gfx/meshimport.h`, which also dispatches on the extension: `mesh::Import`, `IsModelFile`, `Dependencies`).
`gfx::Model::Load` and `gfx::LoadTexture` cache the result and fill rhi staging buffers
(`Buffer::CreateStaging`, before taking a queue's lock), which the staging constructors of `Buffer`/`Image` upload; a
failed load prints why and returns null instead of trapping. When an importer changes what it produces, bump its
`objimport-vN`/`gltfimport-vN`/`imageimport-vN` tag in the loader's params hash, or stale caches keep the old output.
Assets outside `RootPath` are cached under `<user profile>/external/<absolute path>`.

glTF: a scene (`mesh::ImportOptions::scene`, else the default one, or the first; the file's scenes are
`ModelDesc::scenes`, and View > Scene, or `SPEEDO_AUTOLOAD_SCENE=<index>`, loads the file again with another, as a
cache entry of its own; so does a `KHR_materials_variants` variant, `mesh::ImportOptions::variant`, which gives each
primitive its variant's material when it maps one: `ModelDesc::variants`, chosen as the file opens or by
`SPEEDO_AUTOLOAD_VARIANT=<index>`, the default materials without; several laid out to compare: see "A file's material
variants, chosen as it opens", below) is flattened into one mesh with the node transforms applied (a mirroring one reverses the
winding *and* must flip the cofactor normal matrix back, which `NegativeScaleTest` catches). `EXT_mesh_gpu_instancing`
is drawn instanced instead: the node's primitives once, in its space, and a transform per instance (the node's times
the instance's translation * rotation * scale) in `Mesh::instances`/`ModelDesc::instances`, which each submesh names a
range of (`firstInstance`, `instanceCount`; instance 0 is the identity, for everything flattened). Each `Model` has an
instance buffer (`ModelInstance`: the transform and its inverse transpose), which `InstallModel` binds as
`gModelInstances`; a submesh is one `DrawIndexed` of its instances, read at `modelInstanceId + SV_InstanceID`. The
mirroring instances (negative determinant) are sorted last (`mirroredInstanceCount`) and drawn separately with clockwise
front faces (`CommandEncoder::SetFrontFace`, dynamic state like the cull mode): they reverse the winding, so a single
draw would cull them, and get double sided shading's front and back the wrong way round.
Points and lines (strips and loops become line lists) are submeshes of their own (`ModelSubmesh::topology`), drawn
with a pipeline per topology: `Pipeline::BindPipelineAuto(cmd, variant)` takes it as a parameter, part of the pipeline
cache key, rather than as state, since the draw threads share the pipeline. Without normals in the file they keep zero
normals, which the fragment shader draws unlit (base color plus emissive, as gltf says). `VertexMain` writes the point
size (Vulkan needs it for point lists) in a struct of its own (`VertexMainOutput`): slang refuses `SV_PointSize` as a
fragment input. Double sided materials
(`ModelMaterial::doubleSided`) are drawn with culling off, set per submesh with `CommandEncoder::SetCullMode` (dynamic
cull mode, `VK_EXT_extended_dynamic_state`, a required device extension), and the fragment shader flips the normal of
back faces (`SV_IsFrontFace`). Alpha modes become
`mesh::Material::alphaCutoff` (`MaterialData::alphaCutoff`, 0 for OPAQUE, which must not alpha test the base color
texture, and for BLEND). BLEND materials (`blend`) are exact order independent transparency, by per pixel linked lists:
drawn after a view's opaque submeshes, unsorted, with the main layout's second fragment entry point
(`FragmentTransparent`, `[earlydepthstencil]`: its writes must only happen for fragments that pass the depth test, which
the opaque `FragmentMain` can't force, since alpha masked fragments must not write depth) and no color writes (every
`BlendMode` `kNone`, so no depth writes either). Each fragment takes a node (`OitNode`, 12 bytes: premultiplied color as
R11G11B10, alpha and 24 bits of the distance, the reversed depth's float bits, the next node) from `gOitNodes` by `gOitCounter` and pushes it on its pixel's list
(`gOitHeads`, `InterlockedExchange`); `ComputeMain` keeps the pixel's 16 nearest layers sorted (insertion) and blends
them back to front over the opaque color or the backdrop. The buffers are the frame graph's transients, sized with the
render target (4 nodes per pixel, past which fragments are dropped), and cleared before each main pass
(`CommandEncoder::FillBuffer`, in a pass of its own). Fragment
stores and atomics need the device's `fragmentStoresAndAtomics` (the device enables every feature it has). A
`GraphicsPipelineVariant` names its topology, a blend mode per color attachment (by the render target's color attachment
count; it writes depth only if one of them is `kOpaque`) and which of the layout's fragment entry points it runs
(`fragmentShader`, by their order in it: a layout's graphics entry points are all its stages otherwise). The pipeline variant is a parameter of `BindPipelineAuto`, part of the pipeline cache key. glTF texcoords already have v = 0 at the top, so unlike obj they aren't flipped, and
normal maps share the obj convention (with `normalTexture.scale` applied to their x and y, as the spec defines it).
Emissive (gltf `emissiveFactor` times `KHR_materials_emissive_strength` and the srgb `emissiveTexture`, obj `Ke` and
`map_Ke`) is added after the lighting, unclamped (CornellBox's lamp, `Ke 17 12 4`, saturates to white); an emissive
texture with a black factor isn't loaded, but an obj's `map_Ke` with `Ke 0` is the map alone, as exporters write it
(`objimport-v9`: Bistro's hanging lanterns, its shop signs and every lamp of its interior emitted nothing by the
factor). A sampled emissive texture is taken less its black level (`EmissiveTexel`, `kEmissiveBlack`: 4 of 255 in
srgb, 0.00122 linear, wherever one is sampled: the fragments', the voxels', the rays' hits', the emitters' points'): an
8 bit texture's black isn't 0 once compressed (BC1's about 3 of 255 of an authored 4, Basis's 1 of 255), and no
authored emission is that dim; CarConcept's rims, mirror, brake, trim and interior frames have the Khronos logo on a
near black field for their emissive texture with a factor of 1, glowed at 3e-4, a thousandth of a sunlit white, and the
night's auto exposure, +7.7 at its headlights' pools, showed them as a mid gray against the black paint (the user's
white rims and interior, 2026-11-05; the glTF folder's png, 4 of 255 through BC1, drew them purple gray, the GLB's
KTX2 neutral). Occlusion maps (gltf `occlusionTexture`,
by its strength) darken only the ambient term, the stand-in for the indirect light gltf applies them to; they are
imported as `image::Usage::kOcclusion` (the red channel, linear, BC4), since gltf often packs occlusion, roughness and
metallic into one texture's r, g and b, which kMask's luminance would mix. Each texture is a `TextureRef` (`gfx/textureref.h`: path, texcoord set, `KHR_texture_transform` as a 2x3 matrix, and its
sampler as `rhi::SamplerDesc`), which the shader applies: vertices keep both texcoord sets as they are (`texCoord01.xy`,
`.zw`), materials name a `TextureView` per texture (`gTextureViews`: texture and sampler slot, set, transform; 0 is
material 0's, deduplicated per model), and `ViewTexCoord`/`SampleView` sample through them (the normal map's tangent
frame follows its own transformed texcoords). A model's distinct samplers get the 62 sampler slots other than the
default's and the clamping one's (`kModelSamplerSlots`; `ManySamplers.gltf` uses 36), and the previous model's go back to the default. Images embedded in buffers or data uris are named by their model file and
index (`TextureRef::embeddedImage`): `LoadTexture` caches them against the model file and, on a miss, reads their
bytes with `gltf::EmbeddedImage` and decodes them from memory (`image::Import`'s byte overload, which tells the formats
apart by their signature), so nothing is extracted to disk. Compressed meshes are decoded at import: meshopt (EXT_meshopt_compression, which cgltf
parses, and KHR_meshopt_compression, read from its json) into `cgltf_buffer_view::data` right after the buffers load
(`DecodeMeshopt`), so accessors read them as usual; draco per primitive (`DecodeDraco`: cgltf turns the extension's
draco attribute ids into accessor pointers by index), whose values `addPrimitive` reads in place of the accessors'.
Textures use their plain image if they have one, else their KHR_texture_basisu (KTX2) or EXT_texture_webp image, which
`image::Import` decodes: a KTX2 texture with a full mip chain keeps its own blocks where it can (normal maps: Basis transcoded to BC7, y as the
file has it, up the image, which the shader flips by `TEXTURE_VIEW_FLAG_NORMAL_Y_UP`, `Image::normalYUp`, since BC7
blocks can't be flipped and Basis's BC5 takes y from alpha; occlusion: Basis to BC4 from red; metallic-roughness: Basis
to BC7 with the view swizzled to roughness in r and metallic in g, `Image::channels` and rhi's
`ImageViewCreateDesc::components`; each at least as good as decoding and compressing again, by `assettest`'s psnr over
the channels the shader samples), and a color (or linear) one
(`ImportKtx2`: Basis Universal transcoded straight to BC7, or BC1, BC3 or BC7 data as it is, with the file's mips;
`image::Format::kBC7`, which only comes from there; `Image::ownBlocks`: assettest decodes BC7 with bcdec, a header-only
`ports/bcdec` overlay, and only warns about such images' compression and mips, which are the file's), other KTX2 textures (normal maps and the other swizzled usages) are transcoded to rgba8 and compressed
as any other, as are WebP images (libwebp, the first frame of an animation); the decoders' versions are part of those files' cache keys, and draco's and
meshoptimizer's of every gltf model's. KHR_node_visibility hides nodes; morph targets are applied at their default weights (the
node's, else the mesh's; position, normal and tangent deltas) unless an animation moves the node's weights. Skins and node animations (translation, rotation, scale; step,
linear and cubic spline) are drawn moving (`SceneAnimationData`, `ModelDesc::animation`, `gfx/sceneanimation.h`): the
nodes animations move, and their descendants, keep their meshes in node space, drawn with instances linked to the
node (`SceneInstanceLink`), and skinned meshes keep their vertices in mesh space with a `SkinVertex` each
(`gSkinVertices`: 4 joints and unorm16 weights), blended in the vertex shader by `gJointMatrices` from
`PushConstants::jointBase` (world(joint) * inverse bind). Everything else is flattened as before. A moving model has a
host visible instance and joint buffer per frame (`gModelInstances[frame]`, `gJointMatrices[frame]`), which
`Model::Animate` writes on the draw thread after the frame's fence wait, from `EvaluateNodes` on the cpu; a static one
binds its device local instance buffer in every slot, and the defaults (`gDefaultSkinVerticesUuid`, `gDefaultJointsUuid`, `gDefaultMorphDeltasUuid`, `gDefaultMorphWeightsUuid`)
stand in for what it doesn't have. Skinned bounds are at the rest pose (`RestJoints`,
`SkinPosition`): bind space can be far off (CesiumMan's root rotation). View > Animation picks the animation (the
first plays on load), crossfading from the previous one over 0.3 s (`EvaluateNodes` with two `ScenePose`s and a weight:
local transforms lerped and slerped, then composed), pauses and restarts it, and `SPEEDO_ANIMATION_TIME=<seconds>` freezes it (for screenshots). Animated
morph targets (a node with a weights channel) keep their deltas, per vertex a row of a `MorphDelta` per target transformed
like the vertex (`gMorphDeltas`, from `PushConstants::morphDeltaBase` for the vertices from `morphFirstVertex`), added in
the vertex shader before skinning by the frame's weights (`gMorphWeights[frame]` from `morphWeightBase`, a `SceneMorph`
per node), which `Model::Animate` writes from `EvaluateWeights` (crossfaded like the nodes); the bounds cover each
vertex's reach with the weights from 0 to 1. A weights channel's output accessor is scalars, weightCount per key (its
`count` is not the key count). KHR_animation_pointer: cgltf skips a channel target's extensions, so the importer reads
the pointers from the json with cgltf's jsmn (`AnimationPointers`) and parses them (`ParsePointer`). Node translation,
rotation, scale and weights pointers become ordinary channels; the rest get a `ScenePointerTarget` each, whose values (at
rest `pointerDefaults`) `EvaluatePointers` samples like morph weights (`Path::kPointer`, the channel's node is the target).
A node whose KHR_node_visibility animates is kept in its space (instance links, even if hidden at rest), and
`NodeVisibility` hides it and its descendants (`WriteInstances` collapses their instances to a point); lights under nodes
that move or hide follow them (`SceneLightLink`, `WriteLights`). Material values (`MaterialProperty`: the factors of the
core material and of the KHR_materials_* extensions) and texture transforms (a `MaterialTexture`'s offset, rotation and
scale, three targets from the offset's, which `TextureRef::animatedTransform` names: its view is its own, see
`TextureViewKey::animated`) are applied on the draw thread after `Model::Animate` (`ApplyPointerValues`), which patches
the uploaded `MaterialData` and `TextureView`s and uploads what changed. A material with an animated base color factor
keeps it in `MaterialData::color` rather than baked into the vertex colors. Lights' values (`LightProperty`, by the file's light, `SceneLight::source`: one light
can be on several nodes) and cameras' (`CameraProperty`, `SceneCamera::source`) are pointer targets too; a camera is
`animated` if its node moves or its values do, and `Model::Animate` updates the lights (`ApplyLightPointers`, then
`WriteLights`) and cameras (`ApplyCameraPointers`, `WriteCameras`); the draw thread uploads the lights and hands the
cameras to `Views::UpdateSceneCameras`, which re-applies the one the views look through if it animates (not their move
speed). The emissive factor and `KHR_materials_emissive_strength` are a pair of targets (the factor's, then the
strength's) whenever either animates, since the shader's emissive is their product; the anisotropy rotation becomes
its cosine and sine. A layer whose factor animates up from 0 is on (`LayersOf`: its textures load, its flags are set,
and transmission draws in the second phase). `AnimatedBounds` grows a model's bounds by its animations' poses (each
sampled 30 times a second, at most 600: the linked instances' vertex bounds and the per joint bounds of skinned
vertices), so the views frame all of it. `resources/test-models/gltf/AnimatedCameraLight.gltf` animates a camera's node
and yfov, a light's color and intensity, and an emissive strength. A set of files moves as its files do (see "Archives
of several models", below). Cameras are imported (`SceneCamera`, `ModelDesc::cameras`: world position and forward,
perspective field of view or orthographic height, near and far; none for a set of files) and the views use the first
(`Views::SetScene`); View > Camera picks another or frames the model (`Views::UseSceneCamera`). A camera's roll is
`cameraRotation.z` (applied before pitch and yaw), and its aspect ratio (gltf `aspectRatio`, or `xmag / ymag`)
letterboxes its view in its grid cell (`Views::InternalLayout`; the draw uses `Views::GetViewports`). The Khronos glTF-Sample-Assets `Models/` are the test set (see below; `assettest` takes
`.gltf`/`.glb`): there, in the image checks, a 4x4-or-smaller mip only warns about its average (one BC1 block can't hold
more than four colors), and normals below the surface (z < 0, which BC5 can't store) are compared mirrored and warned
about, since both are properties of the asset rather than importer errors.

The loaders don't wait for their uploads: they return the resource with its `gfx::Upload` (the transfer timeline
semaphore and value, and the transfer queue family), and keep it alive from the upload's timeline callback in case the
caller drops it. Whatever uses it on another queue must wait for the upload on the gpu and acquire the resource for its
family (`TransitionThenBind` does both, with `CommandEncoder::AcquireOwnership`): buffers and images are
`VK_SHARING_MODE_EXCLUSIVE`, and the loaders release them to the graphics family. On a single family device (e.g.
KosmicKrisp) the release and acquire are no-ops and the graphics timeline wait covers the upload, so none of this can
be tested there. The transfer queues' timeline callbacks run every frame (in `Draw`) when they aren't the graphics
queues. The staging constructors of `Image` leave it in `kTransferDestination`, and track that: transitioning an
uploaded image from the `kUndefined` it was created with would let the driver discard its contents.

OBJ has no up axis or handedness, so the importer goes by the conventions the renderer expects: counter-clockwise front
faces (the projection flips y, see `Camera`), and `v` flipped so `v = 0` is the first image row. Runs of faces whose
winding disagrees with the file's normals are flipped (mirrored exports), and missing normals are generated, smoothed
within a smoothing group, or within 60 degrees for group 0, from every face around the corner's position. So are all of
a mesh's where its file normals don't follow its faces (`stats.normalAlignment`, the area weighted cosine between a face
and its vertex normals after the flips, below a half, `kMinNormalAlignment`): erato's are turned 90 degrees about y from
its faces, exported in another frame than its positions (0.14; a sign test agreed on three quarters of its area), and
the indirect lighting looked along them into the statue, its robe dark and blotched, changing as the camera moved the
clips (the user's pose `-0.8296,17.5449,19.0786,0.000,0.000`). The rest of the McGuire set is at 0.76 or more, most at 1;
per part, foliage's normals bent up and out on purpose fell below (scrubPine, San Miguel's trees, 264 of Bistro's parts),
so it is the whole mesh's (`objimport-v8`). Some test scenes are z up (e.g. chestnut) and are loaded as
is. Faces are triangulated by the importer, not tinyobj (`LoadObj`'s triangulate off): by halving (`ForEachFaceTriangle`:
the triangle of a face's ends and its middle corner, then each side's), which leaves triangles and quads as a fan makes
them. A fan of a face of hundreds of corners is slivers that all meet at one corner, which no BVH separates (a tree too
deep for tinybvh's build and the traversal's stack), and tinyobj's own triangulation dropped most of such faces:
`geodesic_dual_classII_20_20.obj` came in with 825000 of its 4.58 M triangles (`objimport-v6`).

Rendering is linear: color textures are loaded with srgb formats (`gfx::image::Usage::kColor`, mips filtered in linear
space), the main render target is `R16G16B16A16_SFLOAT`, and `ComputeMain` scales it by the exposure (View > Exposure,
in stops), tonemaps it (`PushConstants::tonemapper`, View > Tonemapper or `SPEEDO_TONEMAPPER`: Khronos PBR Neutral, the
default, which leaves colors up to about 0.76 as they are; ACES, Hill's fit; AgX, Wrensch's fit of the base look;
Reinhard on luminance; linear) and applies the srgb curve when it copies to the swapchain, which stays unorm since it is a
storage image (and imgui's colors are srgb already). Auto exposure (View > Auto Exposure, on by default since 2026-10-26, the user's call with the sky's time of
day; `SPEEDO_AUTO_EXPOSURE=0` without, which captures compared at a fixed exposure need) needs no pass of its own: ComputeMain counts the scene's pixels by their log2 luminance, before exposure,
into 96 bins of 40 stops (`gExposureHistogram`, `SHADER_TYPES_EXPOSURE_BINS`, from `SHADER_TYPES_EXPOSURE_LOG2_MIN`,
-28, a starlit ground, to 12; 64 bins from -12 until 2026-11-05, where a moonlit ground at -21 read as -12 and the
exposure never followed the moon down; per frame index, host visible: shared memory per group, then atomics; emptied with
`FillBuffer` before, and a barrier to the host stage after), and `UpdateAutoExposure` reads a frame's histogram after
its fence (where `Model::Animate` writes the frame's buffers), averages the log luminance between the 50th and 95th
percentile, each pixel weighing whole in the eye's photopic range (above `kPhotopicLog2`, 2^-14, 3 cd/m^2: every daylit
surface, a lit street) and below it by its luminance's share of that level (the mesopic range: a dim street between
its lamps, a twilight sky, a moonlit ground), where that weight is `kLitShare` (3%) of the view or more, blended in
stops from the plain percentiles' mean below it, so that at night the lit parts govern: by the whole view's
percentiles a lit street under a night sky seen from above, the sky and the unlit roofs over half the view, the user's
pose `543.0920,1718.0579,716.2755,-20.047,-76.637` at 21:52 on the 21st of June, read as moonlight, a mean of -19
where its facades are lit at 2^-11, and they came out 8 times white (the histogram's floor at -12 had hidden it); a
view with nothing lit, moonlight alone, keys on the whole view; the day is photopic throughout, unchanged (Sponza's
darkest twentieth by day weighs 0.7). A step at a level (whole above 2^-16, none below 2^-20) counted a dusk sky whole
until it fell out, and the exposure rose 1.3 stops as the sky dimmed and fell back 2 once it was gone, a dip in a time
lapse; by the luminance the dip is half a stop. `SPEEDO_EXPOSURE_LOG=1` prints the percentiles, the lit share, the
mean and the target every 30th frame), and moves the exposure toward `log2(0.2) - mean`, less up to 2 stops as the mean falls from daylight's
(2^-4 in the renderer's units: 2900 cd/m^2 scaled as the sky's sun is, EV100 14, Bistro's street at noon -4.2) to a lit
street's at night (2^-11, 22 cd/m^2, EV100 7; Bistro's -10.5), and `kMoonlightStops` (1.3) more on down to moonlight
(2^-21, a full moon's ground: about 0.02 linear then, a dark gray; 3 more stops left it black but for the highlights):
a night darker than a day, as the eye and a photographer
have it (one key of 0.3 lifted Bistro's night 8.7 stops, to look as bright as noon at 2.5), by an exponential
decay of 2 per second, of the time elapsed whatever it is, so it settles in the same time at any frame rate (fifo at 120
Hz and mailbox at 220 followed the decay from the same targets to 2%; a frame over a second used to take the target at
once). What it measures needn't: the indirect lighting settles by frames (its hysteresis), so a scene's first fraction
of a second differs between frame rates. The Exposure slider is added on top. Each of split screen's views (at most
`SHADER_TYPES_SPLIT_VIEW_COUNT`, 4) has its own: its own bins (`SHADER_TYPES_FRAME_HISTOGRAM_BINS` a frame; the group's
shared histograms per view, since a group can straddle two; a letterboxed pixel counts in none), its own exposure
(`ViewData::exposure`, which every pass that exposes reads for its pixel's view: the composite, Sharpen, the upscaler's
`Exposure()`, the emitters' roulette and the firefly clamp; `PushConstants::exposure` is the first view's) and look
distance (`FrameInputs::lookDistances`), and a view's first measure taken as it is: one exposure for all, a view of a
sunlit roof beside one of a dark corridor blew the roof out. A single view is as before (Sponza +7.49 EV, 22.07 ms).
Shading is the glTF metallic-roughness brdf (`Shade` in the shaders: GGX, height correlated Smith, Schlick) over the
lights in `gLights` (`PushConstants::lightCount`; a model's KHR_lights_punctual lights, `ModelDesc::lights`, in lux and
candela, then the environment's dominant lights), plus image based lighting that occlusion darkens. The environment
(`gfx/environment.h`, cpu only) is an equirectangular panorama (+y up, -z at its center), a Radiance `.hdr` file
(`environment::Import`, scaled to 2048 wide) or, by default and in automated runs, the sky (see "The sky", below: Earth's
atmosphere, its sun where the time of day puts it). A sun disk drawn into the sky's panorama (`environment::AddSun`, 4x4
samples per texel, scaled to exactly its irradiance) tests the dominant light extraction. A panorama's strongest compact light sources are its dominant lights (not
necessarily a sun: a softbox in neutral.hdr, windows in ennis.hdr and papermill.hdr), which
`environment::ExtractDominantLights` takes out, up to `kMaxDominantLights` (4), each found in what the ones before left
(its brightest texel, if 50 times the mean of what is left, and the texels around it within 15 degrees brighter than 2%
of it, flood filled; they get the mean radiance of the texels bordering them, and the rest is the light's irradiance,
kept if at least 5% of pi times the whole panorama's mean), then sorted by irradiance (the brightest texel isn't always
the most light: ennis's second is its window, 2.5 lux against the first's 0.7). They light the scene as directional
lights instead (`Environment::dominantLights`, `SyncLights`: after the model's lights, turned by the environment's
rotation and scaled by its intensity, uploaded when they change; before any environment is installed, the procedural
sky's sun), so they cast shadows (by the shadow budget: a directional light takes a quarter of the atlas) and aren't
counted twice. View > Environment lists them. Where one of the model's directional lights shines from within 5 degrees of
one (checked each frame, after the rotation), they are the same light, merged: the model's alone is kept (as authored, maybe animated), and View > Environment says so (and
turns merging off and on, `gMergeLights`; `SPEEDO_MERGE_LIGHTS=0` starts without). The
Khronos panoramas' bright sources are clipped, so most come out at 3 to 5% of the mean (field, footprint_court, doge2
and pisa have none; papermill has 4, all windows); the sky's sun is 57%, and `assettest` checks it is the one
dominant light found, where and as bright as it was put (and the lighting levels against the panorama's mean less the
dominant lights' share). The client prefilters on the gpu (`gfx::EnvironmentFilter`, `environment.slang`, a layout of its
own, "Environment", kernels selected by `ComputePipelineVariant`): `LoadEnvironment` decodes a file and takes its dominant
lights out on the cpu (cached, `environment-vN`: the original and lighting panoramas as half floats, and the lights), and
uploads both panoramas; the sky needs nothing (the filter draws its source, `EnvironmentSky`, and its sun is known).
`InstallEnvironment` then records the filter on the draw thread (after the uploads are acquired, writing the rest of
`gEnvironment` first): the source pyramid (`EnvironmentDownsample`, by solid angle, down to 8 wide), level 0 (the original,
or the sky's source), the GGX and Charlie levels, and the irradiance (its rows' partials first, `EnvironmentIrradianceRows`: a thread per
spherical harmonics coefficient and row of the source mip at most 128 wide, into an image of floats a column per
coefficient; then one group summing the columns into `gEnvironment[0]`; one thread per coefficient over the whole mip,
8192 dependent loads, took about a millisecond and a half of every sky redraw), each kernel over
a mip as a storage image (`ImageViewCreateDesc::baseLevel`) and dispatched by threads (`CommandEncoder::DispatchThreads`,
by the reflected group size). The resources of a layout of its own start at set 1: set 0 is the push constants'
(`[[vk::binding(0, 0)]]` on them, as in `Main`), else they share binding 0 with a resource. `assettest` checks the same
kernels, run on the cpu: slang compiles `environment.slang` to c++ at build time (`slangc -target cpp`, a custom command
of the assettest target), and `src/tools/environmentkernels*` run it in `EnvironmentFilter::Record`'s order, with cpu
textures behind the prelude's `ITexture` (trilinear, around in u and clamped in v, as the gpu's sampler) and rows of
groups on threads. A group's threads run one after another there, so the kernels can't use shared memory or group
barriers (the irradiance is a thread per coefficient and row, then per coefficient, through an image of partials). The prelude's global names (`half`, `Vector`, ...) stay behind
`environmentkernelsbridge.cpp` (plain types), away from the gfx headers. It included `<stdfloat>` for c++23 and up,
which libc++ lacks in every language mode: `ports/shader-slang/0005-*` includes it only if `__has_include` finds it (to
go to the fork, as 0001 to 0004 did). libc++ runs
`std::execution::par` serially, hence the threads (the old cpu prefilter took 4 s a panorama, the kernels 1 s). The
kernels turn the panorama without its dominant lights into 6 levels of GGX prefiltered radiance (roughness `i / 5`, the split
sum with n = v = r, filtered importance sampling from a mip pyramid; level 0 is the panorama, dominant lights included:
the backdrop, and mirrors), the mips of one `R16G16B16A16_SFLOAT` texture
(`SHADER_TYPES_ENVIRONMENT_TEXTURE`), 6 levels prefiltered with the sheen's Charlie lobe (`Environment::sheenLevels`, from
an eighth of the width, `SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE`, `EnvironmentData::sheenLevelCount`), and the irradiance as 9 spherical harmonics coefficients (`EnvironmentData` in
`gEnvironment`). The shader takes the specular's second term from Karis' environment brdf fit, and looks the base and clearcoat
specular up along Frostbite's dominant direction (`DominantDirection`: leaned from the reflection toward the normal by
roughness, which keeps rough grazing reflections from skimming the horizon). The Charlie lobe can't be importance
sampled the usual way (Estevez and Kulla's half vectors): at low roughness its weight is near the horizon, where most
reflected samples land below it, and the levels' means drifted by up to 50%; `PrefilterSheenTexel` samples l over the
hemisphere with `cos = u^k` instead, k growing as the roughness drops, weighted by the lobe over that pdf. Its intensity and rotation (about
+y) are push constants (`PushConstants::environmentIntensity`/`environmentRotation`, from View > Environment, or
`SPEEDO_ENVIRONMENT_INTENSITY`/`SPEEDO_ENVIRONMENT_ROTATION` in degrees), applied to every lookup
(`EnvironmentDirection`). It is the backdrop too: the main color target is cleared to alpha 0 and opaque draws write
alpha 1, so `ComputeMain` knows where nothing is, finds the view the pixel is in (`ViewData::viewport`, `viewCount`)
and samples the panorama along the ray through it (`ViewData::inverseViewProjection`, unprojected at depths 1, the near plane,
and 0.5, which works for infinite far planes and orthographic views too). Outside the views (letterboxing) the clear color
stays. Equirectangular rows near the
poles cover less of the sphere: the pyramid's mips weigh rows by their sine, and a sample's mip goes by the texel's real
solid angle, otherwise the rough levels drift from the panorama's mean (papermill's by 15%), which `assettest` checks
(each level's mean within 3%, the irradiance's within 2%, and each level smoother than the one before). Image views
cover all of their image's mips unless `ImageViewCreateDesc::levelCount` says otherwise (they used to cover only level
0, so no texture was ever sampled below it). Metallic-roughness textures are `Usage::kMetallicRoughness` (BC5: roughness, the file's
green, in r, and metallic, its blue, in g); obj materials are dielectrics whose `Ks` scales the specular
(`MaterialData::specular`, also KHR_materials_specular's factor; 0 is matte, without even a fresnel rim) and whose `Ns`
gives the roughness (`sqrt(2 / (Ns + 2))`), and `MaterialData` starts from `DefaultMaterialData()` (roughness, specular
and specular color 1, ior 1.5: zero roughness is a mirror, and a zero specular color no fresnel at all). `Shade` lights a
`Surface` (diffuse color, f0 color, f90, roughness, occlusion, normal), which `ShadeFragment` builds as the spec does: the
dielectric's f0 is `((ior - 1) / (ior + 1))^2` (KHR_materials_ior) times KHR_materials_specular's color (factor times its
srgb texture), at most 1, times the strength (factor times its texture's alpha, `Usage::kAlpha`: BC4), and its f90 the
strength, mixed with the metal's (f0 the base color, f90 1) by metallic; the environment term is `f0 * A + f90 * B`.
Specular-glossiness materials (`MATERIAL_FLAG_SPECULAR_GLOSSINESS`) take their specular color as f0 (f90 1), their
diffuse scaled by 1 minus its largest component, and roughness `1 - glossiness * a` of the specular-glossiness texture
(in the specular color's slot, `kColor`: srgb rgb, linear alpha). KHR_materials_sheen adds a Charlie lobe (Estevez
and Kulla's visibility, as the Khronos sample viewer has it) over that, which scales the layers below by
`1 - max(sheen color) * E`, E its directional albedo (`kSheenAlbedo`: a 12x12 table over cos and roughness, integrated
numerically and clamped to 1; lit by the environment's sheen levels along the reflection, times E). KHR_materials_clearcoat adds a dielectric GGX layer
(f0 0.04) of its strength (`kOcclusion`: the texture's red), roughness (`kMetallicRoughness`: its green) and own normal
map (`NormalFromMap`, else the geometry's normal) over everything, emission included, which keeps `1 - strength *
F(n_c . v)`. KHR_materials_transmission replaces that much of the dielectric's diffuse with the light from behind
(`TransmittedLight`, as the Khronos sample viewer): the main pass draws in two phases when the model has transmissive
(not blended) materials (`MainPassPhase`): the other opaque submeshes, then, after the Transmission pass copies the color
to the transmission texture (`SHADER_TYPES_TRANSMISSION_TEXTURE`, a full mip chain: `ComputeTransmissionCopy` into level
0, a storage view of it, and the Transmission Mips pass's `Image::GenerateMips`, which blits each level from the one above
with per level barriers), the transmissive and blended ones, loading the attachments. The copy is compressed by each
texel's brightest channel at the view's exposure, `c / (1 + max(c) e)`, and the lookup takes it back, `c / (1 - max(c) e)`
(`TransmissionCompress`, `TransmissionDecompress`: Karis' reversible tonemap, as bloom's downsampling has it), so the mips
average texels far brighter than white as about white: CommercialRefrigerator's frosted door lit a blob over the top shelf's
bottles, its shape the frost's rough parts, every 18 frames at balanced (8 at native), where the sun's glint along the
shelf's front wire, thinner than a pixel, was hit by 2 of the jitter's 18 points (5 to 20 times white) and the rough
frost's blur spread it (the user's pose `-0.3220,1.1347,1.1294,-6.897,0.448`: the region 91.6 of 255 at that jitter point
against 36.5 at another, now 34.7 and 34.9; its flashing pixels with the door closed 18524 over 60 frames, now 597). The
glass sample models render the same to 0.13 of 255 (StainedGlassLamp's beads beside its bulb a little less lit). The refracted ray (by the ior) leaves the volume after the thickness
(KHR_materials_volume: factor times the texture's green, times the instance's scale; 0 is thin walled. the thickness is in the mesh's units, so a scaled node with a volume material is drawn as an instance of it, not flattened, for its instance to carry the scale: DragonAttenuation's dragon, under a node scaled by a quarter, came out deep red, four times as thick), is projected into
the view (fading to the environment along the ray over the view's last 5% and beyond it, where nothing was drawn:
clamping streaked the edge's texels, stretching as glass neared a corner; offsets shortened to stay inside, or mirrored
in, looked no better, and a guard band costs too much), and samples the texture at
`log2(width) * roughness * saturate(2 ior - 2)` through the clamping sampler
(`SHADER_TYPES_CLAMP_SAMPLER`, a reserved slot, so models get 62); where nothing opaque was drawn the environment along
the ray fills in by the texture's alpha, which is why the color target is cleared to transparent black (ComputeMain
draws the gray clear color itself where neither the views nor the backdrop are). The volume attenuates it by
`attenuationColor^(thickness / attenuationDistance)`, and KHR_materials_dispersion samples each color with its own ior
(spread by `(ior - 1) * 0.025 * dispersion`). KHR_materials_anisotropy stretches the GGX lobe along a direction in the
tangent frame (`SurfaceFrame`: the vertex tangents', else from derivatives; the texture's rg along +u and up the image,
glTF's bitangent, i.e. -v as sampled; `kLinear`), turned by the rotation: alpha along it `lerp(alpha, 1, strength^2)`,
and the environment is reflected about a bent normal (as the Khronos sample viewer). KHR_materials_iridescence's thin
film Fresnel (`IridescenceFresnel`, Belcour and Barla) is evaluated once per pixel at the view's angle over the
surface's f0 and mixed into the Fresnel of the environment and the lights by its strength. KHR_materials_diffuse_transmission
lights that much of the diffuse from the back (lights behind the surface, and the irradiance around -n), tinted by its
color alone, as the spec's `diffuse_btdf(diffuseTransmissionColor)` has it (it was multiplied by the base color too,
which turned DiffuseTransmissionTest's white and red quads at factor 1 dark teal), and weighed as the dielectric's diffuse
is (`1 - metallic`, `1 - transmission`); its strength texture is the alpha channel (`Usage::kAlpha`). KHR_materials_retroreflection (the minimal retroreflective microfacet model) blends the base lobe (`BaseLobe`: diffuse and specular, not sheen, clearcoat or transmission) with the same lobe seen from the view mirrored about the normal, `2 (n.v) n - v`, by its weight (factor times the texture's red, `kOcclusion`), which turns the specular back towards the light; the environment's retroreflective lookup is along the view itself (the mirrored view's reflection), and n.v, so the split sum's terms, don't change. It is one of the extended layers (`SHADER_TYPES_LAYERS_EXTENDED`). KHR_materials_volume_scatter is screen space subsurface scattering, after the Khronos sample renderer's but by the
extension's definitions (the renderer's own approximation drew ScatteringSkull pale, flat and barely blurred). Three passes
over the scattering materials' triangles (`BuildScatterDrawList`): Scatter Back draws their back faces' depth
(`BuildScatterBackDrawList`: single sided ones, front faces culled, `FragmentShadow`;
`SHADER_TYPES_SCATTER_BACK_DEPTH_TEXTURE`); Scatter (`FragmentScatter`, the layout's fourth fragment entry point) draws
the light coming out of them (`ScatterLight`): what enters from the front in the multiscatter color (the albedo of all of
the scattering, which the path tracers' single scattering albedo, `1 - (4.09712 + ...)^2`, is made to produce: tinting by
that is what made it pale), the environment's by occlusion, plus what comes through from behind, attenuated over the
thickness along the view to the nearest back face (the thickness map where there is none: ScatteringSkull's says about 1 m
on a 0.34 m skull), times the diffuse transmission and its color, less the specular's fresnel
(`SHADER_TYPES_SCATTER_TEXTURE`, the material + 1 in alpha, and `SHADER_TYPES_SCATTER_DEPTH_TEXTURE`); Scatter Blur
(`ComputeScatter`, the layout's second compute entry point, once per pixel: in the main pass's fragments it cost every
overdrawn one, nearly doubling Main) blurs it by Christensen and Burley's profile, scaled per color by the mean free path
(the reciprocal of the extinction, `-ln(attenuationColor) / attenuationDistance`, at most 10 attenuation distances) and the
multiscatter albedo (`BurleyScale`, the searchlight fit; the renderer's attenuation distance times the multiscatter color
through Blender's radius convention was a tenth of that), from the 55 samples precomputed as the renderer does, samples of the
same material only, weighed by their world space distance. The pattern is turned by the pixel's place in a 4x4 tile
(fixed, it made copies of the image; turned by interleaved gradient noise, faint stripes), which Scatter Denoise
(`ComputeScatterDenoise`, the third compute entry point) cancels: interleaved sampling, each pixel the mean of its 4x4
tile's neighbors on the same surface (same material, within 8 times their screen distance in world space), as many
samples as the 16 pixels took; the profile's albedo is each pixel's multiscatter color, factor times texture, which the Scatter pass writes as its second
color attachment (`SHADER_TYPES_SCATTER_ALBEDO_TEXTURE`, srgb; `kScatterBlend`), since the blur has no texcoords: a darker
texel absorbs sooner, so it diffuses less (up to 4 times), which `ScatterTexture.gltf` shows (a slab of bright and dark
stripes between slabs of each color, whose stripes must look like them; the material's factor alone smeared the dark
stripes into the bright). It cost nothing measurable, so it isn't a specialization.
The main pass adds it (`SHADER_TYPES_SCATTER_LIGHT_TEXTURE`, else, where the scatter pass didn't draw the surface,
`ScatterLight` in place) times `(1 - metallic) (1 - iridescence) (1 - transmission) (1 - clearcoat fresnel)`; all of the
diffuse transmission's light is the scattering's. The volume (thickness, attenuation) is filled for diffuse transmission
and scattering materials too, and attenuates the diffuse transmission's back light. What it can't do: light crossing
more than the profile's reach, caustics, and the volume's own shadowing.
Every `KHR_materials_*` of the glTF-Sample-Assets is drawn, and `KHR_materials_variants` is chosen at import (see above). gltf primitives without a material get the spec's default (white, metallic 1, roughness 1), a
material of the model's own, while obj faces without one use material 0, which opening an image textures. KHR_materials_unlit draws the base color alone. A model's materials
(`ModelCreateDesc::materials`, drawn per `submeshes`) are materials 1 and up in `gMaterialData`, which starts at
`SHADER_TYPES_MATERIAL_COUNT` (1024) and grows to the next power of two that holds a model's (`GrowMaterials`: a new
buffer, the old one retired after the gpu's work; NodePerformanceTest has 10000 per encoding, and its materials past the
first 1023 used to be drawn with the default one, with a warning; the index used to share a push constant with the view,
10 bits). The scatter pass writes a scattering material's index + 1 to a half float, exact to 2048: one past that is a
hard failure. Their diffuse, alpha
(`map_d`, `kMask`: BC4) and normal (`norm`, `kNormal`, else `map_bump`/`bump`, `kBump`: both BC5) textures are loaded with the model and go in `gTextures` slots from 74
(0 the main color, 1 to 3 temporal upscaling's composite and color, 4 the main depth (the depth pyramid's source), 12 the environment's, 15 the texture of material 0 that opening an image loads, 16 and 17 the menu bar blur's, 18 the pre-pass's normals, 19 to 22 the traced shadows' images, 23 to 25 the indirect lighting near field's, 26 to 29 its history's, 30 to 36 the emitters' and 37 the sky's view of itself, 38 the pre-pass's motion, 39 to 55 temporal upscaling's other images, 56 and 57 the emitters' pixels' surfaces, 58 to 65 the traced glossy reflections', 66 the shadow transmittance, 67 and 68 the reflection rays the visibility buffer's shading records, 69 and 70 the sky's transmittance and multiple scattering tables, 71 the sky's views from the eyes, 72 the sky's view by the moon, 73 the lens pass's image). Bump textures are height maps in
most mtl files, but some are normal maps: the importer tells them apart by color (normal maps are bluish), turns
heights into normals (scaled by `-bm`), and stores all of them with +y along +v as sampled, i.e. down the image (the
obj importer flips v). Vertices carry gltf's tangents (`VertexP3fN3fTa4fT014fC4f::tangent`: xyz along +u, w the
handedness, so the bitangent `cross(n, t) * w` points *up* the image, i.e. along -v in this convention; mirroring node
and instance transforms flip w). gltf triangles with a normal map but no `TANGENT`, and obj faces whose material has a normal or bump map, get
MikkTSpace's (`mesh::GenerateTangents` in `gfx/tangents.h`, the `mikktspace` port; per obj material, whose vertices no
other material shares, since the material is part of the obj vertex key: from the normal map's texcoord set, with v flipped back up the image as the
exporters' MikkTSpace sees it, so that the bitangent points the way gltf's does; a vertex is split where its corners'
tangents differ, and generated tangents are made perpendicular to their own vertex normal, since degenerate triangles
get a neighbor's). Without them (degenerate corners, and models without normal maps) w is 0, and the fragment shader builds the tangent
frame from screen space derivatives instead,
corrected by the sign of `dot(cross(ddx(p), ddy(p)), n)`, which is negative here since the framebuffer's y
points down: without it bumps come out inverted.
Either frame follows the normal map's texture transform. NormalTangentMirrorTest checks the vertex tangent path: it
renders right either way, but negating its tangents' w must break it. A quad with a known height map (a dome, which must be lit on the side
the light comes from) is the quickest way to see a sign error. `InstallModel` switches model, textures and materials in one draw
thread step, after the textures are transitioned: every change to `gTextures` takes a new descriptor set (see above),
and the pool only holds 128 copies of that 1024 slot array. Installing a model also sets the views to its first camera,
or frames them on its bounds (`Views::SetScene`, `Views::FrameBounds`).

File > "Open..." is the file browser, which loads models, zip archives, environments (`.hdr`) and images, by their type
(`LoadAndInstallFile`), and several models at once (side by side or as authored); View > Environment > Open
Environment... is the same browser for panoramas only (`BrowserPurpose::kEnvironment`, with listing options of its own); View > Environment picks the physical sky or opens
a panorama. `SPEEDO_AUTOLOAD_MODEL`, `SPEEDO_AUTOLOAD_IMAGE` and `SPEEDO_AUTOLOAD_ENVIRONMENT` take paths absolute or
relative to the resource directory (without an environment, the sky loads, as one of the autoloads). Zip archives (opened, or a `.zip` in `SPEEDO_AUTOLOAD_MODEL`, which loads all of its models) are extracted
once into `<user profile>/archives/<name>-<hash of path, size and time>` with `core::zip` (`core/zip.h`: our own
reader, libdeflate's inflate and crc-32), then loaded from there like any other files; with several models the user picks one. The extractor reads
each entry by its local header: some archives have stale central directory entries (cube.zip in the McGuire archive),
which unzip ignores too. An entry shorter than a page is cut to its size once written: mio makes a new file a page long, of
whatever its stack held, and only ever grows it (`core::file::Write` cut its files already), so geodesic.zip's small objs
had garbage tails that failed to parse; the `.extracted` marker's length says which extractor made a directory, and an
older one's extraction is done again in place (`ExtractArchive`). `assettest` takes zip archives as well, extracting them the same way.

`scripts/assettest.ps1 <zips or dirs>` (PowerShell, as the other scripts) runs the `assettest` tool (imports every model and image and checks the result:
index ranges, normals, winding, missing textures, mip chains, unwritten blocks, compression error), and with `-Client`
also loads each model in the client (`SPEEDO_AUTOLOAD_EXIT=<frames>` makes it exit after that many drawn frames once the
autoloads have installed and `SPEEDO_CAMERA` has placed the views: counted in the ui's frames, which run ahead of the
draw thread's, a big model's install and a capture of it came apart), and
`SPEEDO_BACKGROUND=1` opens its window behind the others without taking the focus, so the runs don't interrupt whoever
is working, and `SPEEDO_PRESENT_MODE=fifo`: with mailbox, the client's default where the surface had it until 2026-11-01, NVIDIA's Windows
driver hangs about 1 exit in 100 in `vkDestroySwapchainKHR`, which no change of ours fixed: handling the window's messages
meanwhile, or keeping the presents' semaphores until their images were acquired again (which validation then reported
destroyed in use, `VUID-vkDestroySemaphore-semaphore-05149`: without present fences it can't know a present is done);
0 in 858 with fifo), failing
on load errors, asserts and validation messages. The runs `scripts/test-assets/splitscreen.txt` names (by label,
`client-<label>.log`) go again in split screen (`<label>-splitscreen`: `SPEEDO_LAYOUT=2x2 SPEEDO_CAMERA_VIEWS=around`), each
view with its own exposure, shadow cascades and indirect lighting clips: the quick tier's that are in it, the full sets'
all. Every run lays out its models' material variants (the script sets `SPEEDO_AUTOLOAD_VARIANTS=all`, see "A file's
material variants, chosen as it opens"): a gltf sample model's encodings side by side, each with its variants behind it,
CarConcept's, DragonAttenuation's, MaterialsVariantsShoe's, SheenChair's and StainedGlassLamp's in the quick tier, and
ChronographWatch's and GlamVelvetSofa's too in the full sets. Debug clients load the validation layer themselves (in
code, `instance.cpp`, with rhi's object names in their
messages); `-Validate` loads it into a profile or release preset's clients through the environment (`VK_INSTANCE_LAYERS`,
and the settings the code gives it as `VK_KHRONOS_VALIDATION_*` variables), each client's messages to a file of its own,
appended to its log (without a messenger of the client's the layer prints them itself, and only to a file once the crash
reports make one, on NVIDIA), which fails a run on `VUID-`, `UNASSIGNED-`, `Validation Error: ` or `Validation Warning: ` as
a debug client's log does, and on `WARNING-CoreValidation-`, the debug messenger's form of a warning (the ImGui pipeline's
pool lacking a type its layout allocates was only a warning, which neither check caught). The quick tier runs once, on every platform: a profile preset with `-Validate` (the user's call, 2026-11-04:
with ray query clients one at a time on the mac, its clients took 198 s, where profile's took 140 and then debug's
1237). What only a debug client checks (`ASSERT`s, which `NDEBUG` compiles out, and rhi's object tracking and uuid
checks, `SPEEDO_GRAPHICS_VALIDATION_LEVEL`) the full sets still check: the profile preset first, then debug with
`-ClientOnly` and the same `-Work` dir, whose caches it reuses (debug imports slowly). A debug client
compiling the shaders (a new profile, or a changed shader) took more than ten minutes a profile, with no window
meanwhile: SPIRV-Tools' optimizer, which slang runs over every shader (`SLANG_OPTIMIZATION_LEVEL_MAXIMAL`), linked
statically from the debug spirv-tools package at the toolchain's `-Og` without inlining. `ports/shader-slang` and the
`ports/spirv-tools` overlay (of the upstream port, for this alone) build their debug configs with `-O2 -g`
(`OPTIONS_DEBUG`): about 2 minutes a profile now. (Mapping the debug config to the release slang in CMake couldn't pick
it: both dylibs share a name on the executable's rpath, and the debug directory comes first.) The validation layer's
dylib is in `debug/lib`, so a debug client run by hand sets `DYLD_LIBRARY_PATH` to it as the script does;
`VK_LAYER_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED` adds gpu assisted validation. Both run in
parallel: `assettest` checks its models and images on every thread (`CheckInOrder`: results printed in the serial order;
`--jobs N`), and the script runs `-Jobs` clients at once (default 4), each worker with a user profile of its own
(`<work>/user-<n>`: clients write pipeline caches, shader intermediates and asset caches there), a model to the same
worker (FNV-1a of its label), so its caches stay warm, each worker's longest first (by what each took the last time with
the preset, `<work>/logs/<preset>/durations.tsv`), and a worker with none of its own left takes the shortest of the
busiest worker's (the most time left; short, so cheap to load without its caches): assigned once and for all, the
workers that ran out waited for the one with the most, and the end of a run was one worker's alone (the full debug run
took 909 s for 2613 s of clients, of which 4 workers needed 653; with the stealing, on its first run without durations, 843 s for 3333 s, a fourth of which is 833: the stolen runs import again in the other profile, which the durations make the short ones); on macos a new worker profile is an APFS clone (`cp -c`) of
`<work>/user`, the serial runs' profile. Results print in the serial order, and the logs are kept per preset (`<work>/logs/<preset>`: a debug run after a profile one overwrote its logs, so a profile failure couldn't be read). That took the full regression from about 30
minutes to 5 or 6 (130 to 175 s profile, 180 s debug, warm). (Before each client, the script also used to wait a second in `caffeinate -u -t 1` to wake the
display; it wakes it once now, and keeps it awake until it exits, `caffeinate -d -i -w` on itself: kept awake per client,
nothing held it between two, and once every client after the first failed at window creation, `GLFW: No monitor
connected?`, 57 of the quick tier's 58 with the user away.) For iterating, `fetch-test-assets.ps1 -Quick` prints a quick tier instead
(`scripts/test-assets/quick.txt`: a model or two per feature, plus the hand-made ones; about a minute, three and a half with ray query clients one at a time on the mac, validated); the full sets run
before committing. `assettest.ps1` takes `.hdr` panoramas as inputs too, and runs a directory of one model's encodings
given directly (a glTF sample model: its files all named after it) as one set, as when its parent is given. The client's
main loop sleeps in `glfwWaitEvents()`, so anything that must end it from another thread goes through
`RequestExit()`, which posts an empty event.

The test sets come from their sources, not from local copies: `scripts/fetch-test-assets.ps1` downloads Morgan McGuire's
Computer Graphics Archive (obj, about 2.7 GB), the Khronos glTF-Sample-Assets models (at a pinned commit, about
2.3 GB) and the Khronos glTF-Sample-Environments panoramas (about 400 MB; Git LFS files, downloaded from GitHub's LFS
media server at a pinned commit and checked against `scripts/test-assets/environments.txt`, the sizes and sha256s of
their LFS pointers; `assettest` checks each, and `assettest.ps1 -Client` runs the client once per panorama) into `resources/test-assets` (gitignored; the client's file browser opens there; or `$SPEEDO_TEST_ASSETS`), and
prints the paths to test:
`scripts/assettest.ps1 -Client (scripts/fetch-test-assets.ps1)` (from pwsh; from another shell,
`pwsh scripts/assettest.ps1 -Client $(pwsh scripts/fetch-test-assets.ps1)`). The archive publishes no versions or checksums and
does change (several files differ from a 2019 copy), so `scripts/test-assets/mcguire.txt` pins each file's size and
sha256: a changed file is kept as `.unverified` and reported until the manifest is updated, after checking what changed.
`resources/test-models/gltf` holds hand-made models for what no downloaded one covers (`SparseIndices.gltf`: index
accessors that are sparse, with and without base values; cgltf's `cgltf_accessor_read_index` and
`cgltf_accessor_unpack_indices` refuse sparse accessors, so `gltf::ReadIndices` applies them;
`InstancingTransforms.gltf`: instances of a single sided, asymmetric triangle under a scaled and moved node, with
normalized short rotations and a mirroring instance, which must face the camera too; `BlendOrder.gltf`: blended quads
listed nearest first (metallic, so they render dark); `BlendInstances.gltf`: a
blended quad instanced in front of and behind another; `ManySamplers.gltf`: 36 quads with a sampler
each, of every filter and wrap mode, more than the model sampler slots once were; `ScatterTexture.gltf`: volume scatter's
multiscatter texture, see above; `ManyLights.gltf`: 24 point and 8 spot lights between pillars, more shadowed lights than
the atlas holds at full size, best seen with `SPEEDO_ENVIRONMENT_INTENSITY=0.05`; `ThousandsOfLights.gltf`: 3800 small
point lights and 200 spot lights, all ranged, between 625 pillars on a 200 m ground, for clustering and culling at scale,
generated by a seeded script, best seen the same way; `CausticSpheres.gltf`: a clear and an amber glass sphere (ior
1.5, volumes) over a light gray ground, whose caustics land at the edges of their shadows under the default sun;
`VisibilityTiers.gltf`: a plain quad with a small clearcoat quad
in front of it, a quarter and a hundredth of the view framed, whose tiles the visibility buffer sorts, see "The visibility
buffer"; `FogHall.gltf`: a closed hall 202 m long, 40 m wide and 48 m tall, dark inside, lit by the sun through six 2 m
slots across its roof, placed so that their sheets of light cross the camera's height 10, 20 and 30 m either side of
it, the camera at eye height by one wall looking across at the other, the whole turned so that the hall runs along the
default sun's azimuth: light shafts seen from the side in dense fog, see "Fog near the ground" under "The sky"; made by a
small C program, the sheets' geometry worked out in its comments), and is always part
of the printed paths, as is `resources/test-models/obj` (`BumpDomes`: a quad with a bump map of four domes, as is and with
its texture mirrored, which must both come out raised and lit from above, i.e. from the default light's side). Two archive files are both called `sponza.zip` (Crytek's and Dabrovic's), so the latter is saved as `dabrovic_sponza.zip`,
and Bistro's five zips (the scenes and three texture packs, which the scenes reference as `..\BuildingTextures\...`) are
extracted side by side into `mcguire/bistro/`. Known asset problems that only warn: erato's normals don't follow its faces and are
generated (see the obj importer above), Bistro and bmw have normals that get replaced, and
`geodesic_dual_classIII_20_10.obj` has 1419 stray vertices (at radius 1140, the others at 1), which `assettest`'s stray
vertex check (more than 100 times the median distance from the median point; ground planes reach about 20) finds. It and
`geodesic_dual_classII_20_20.obj` (faces of up to 384 corners that overlap: most of its rays meet a triangle tied with
another) make trees so deep and overlapping that a ray visits most of them: with a stack of 24 a ray in 40 dropped what didn't
fit, and a dispatch of such rays ran past Metal's command queue timeout (`VK_ERROR_DEVICE_LOST`, 2 runs in 3 of the
geodesic set). The stacks are 48 and 128 now (`BVH_STACK_SIZE`, `BVH2_STACK_SIZE`: no measurable cost, Bistro 25.13 ms
either way), and a ray stops after `BVH_MAX_STEP_COUNT` nodes (4096; the binary layout 16384), with the nearest of what
it met (`BvhHit::capped`; a capped shadow ray is shadowed), counted (`GI_STATISTIC_CAPPED_BVH_RAYS`, printed): every ray
of the dual 20_20 is capped, no ray of the other test scenes. `CheckBvh` reports capped rays apart, as a warning.
tinybvh's own stack is raised for their builds (`TINYBVH_STACK_SIZE` in `bvh.cpp`: "CombineLeafs, tree too deep" at 128).

**The ground plane** (on by default since 2026-11-04, the user's call: the file browser's "Ground plane", and
`SPEEDO_GROUND_PLANE=0` for none for the autoloads and Paste Pose, which Copy Pose writes; `ModelGround`, `Model::Load`'s
`ground`): a plane under the model to see its shadows and caustics on,
added to its staged triangles after its cache entry is read or its set merged (`AddGround` in `model.cpp`; nothing of it
is cached): a patch around the model's center, 1.25 times its largest dimension from the center to each side (a shadow
of its height falls on it with the sun 40 degrees up), a thousandth of that dimension below its lowest point (a floor of
its own isn't fought), facing up, single sided, of tiles of a round size (1, 2 or 5 times a power of ten, 10 to 25 of them
from the center to a side) aligned to the world's origin; then out to the horizon (the user's call, 2026-11-04, to see the
aerial perspective in the distance), 10 rings around it, each 4 times as far as the one inside, to a million times the
patch: far enough that the sky's air hides its edge, and the views have no far plane where the model stands on it (see
"Depth: reversed"). Its horizon from eye height is a line, as a plain's is (the air hazes the far ground within the
pixel at the line: a pixel below it the ground is 500 m off, through a tenth of the fog's depth along the horizon; the
sky model's own ground is as sharp, a shade darker and warmer than the sky's horizon, which `assettest`'s sky check
prints), which the user found harsh (2026-11-05, with the dark band the fog drew over it then, see "Fog near the
ground"): so the plane is hazed toward what the backdrop shows at the horizon in the pixel's azimuth
(`GroundHorizonHaze` in `fragment.slang`, at the end of `MainColor`, for the materials with a checker; `BackdropHorizon`:
the sky's glow there, `SkyGlow`, the sky's views without the disks and the stars, or a file's panorama, through the fog
along the horizon, `FogToSky`, so the sky's last row and the plane's first are one color) as the fog near the ground
would haze it from the top of its layer: the layer above the eye laid over the ground, exp of its vertical optical
depth from the eye's altitude over the sine of the angle below the horizon, thicker with the fog (Fog Visibility and
Height), none with it off or from above the layer, where the air's own haze is in view; a presentation of the plane
alone, nothing of the scene takes it. At the default fog, 0.026 deep, the plane is three quarters hazed a degree below
the horizon, two fifths at 3 degrees, a seventh at 10 and a twentieth at 30, where a model's patch is (its shadows
lighten by a few thousandths); DamagedHelmet's horizon fades over 6 degrees into the sky's gray blue. A ring's inner edge has the vertices of the edge it meets (the patch's tiles' corners, the ring
inside's outer edge scaled from them), so that no pixel falls between. Light gray, checkered in squares of a unit (the
user's call: they measure the model), drawn by the shading (`MaterialData::checker`, `kGroundChecker`: the mean gray
0.45 times 1 plus or minus a ninth, 0.5 and 0.4, by the parity of the texcoords' squares, which are the place;
`GroundChecker` box filters the square waves' product over the pixel's footprint, Quilez's, so that it fades to its gray
toward the horizon rather than aliasing), and its vertex colors the mean gray, which what the shading doesn't draw sees
(the voxels, the rays' hits); roughness 0.8. Two submeshes and materials of its own, the patch's and the rings' (the same
look; their checker by `Model::GetGroundMaterial` and `GetHorizonMaterial`, its last two, not a field of `ModelMaterial`,
which the model cache serializes), and an instance (the identity), after the model's, so its own are as they were (a
skinned model's skin vertices padded with none). The GI and the shadows are the model's alone, which the ground takes
where they are (the user's call: "only where other geometry exist"): the rings are drawn alone (no shadows cast, no
voxels, not in the BVH nor the ray queries' structures: `Staged::untracedMaterial`, `bvh::Triangles`' and
`RayQueryGeometries`' `untraced`), and the patch casts nothing (`BuildShadowDrawList`) but takes the model's shadows,
caustics and light; the indirect lighting's clipmap covers the model's bounds (`FrameInputs::casterBounds`, its framed
bounds), the ground right under it in the last clip's margin, over which the clips fade into the environment's light
(`GiClipWeight`, `GiSample::held`), so there is no seam; and a point no shadow view holds is lit unless its way to the
light passes the casters' bounds (`ShadowPathMeetsCasters`), else every pixel of ground past the cascades traced a
shadow ray. DamagedHelmet on the ground (2560x1440, KosmicKrisp): 81 ms a frame at first, 156 with the clipmap the
model's alone (no change in its probes or rays: the probes' rays met the rings), 113 without the surface cache, 28 on
the BVHs (`SPEEDO_RAY_QUERY=0`), 20.5 with ray queries and the patch alone: the rings' triangles, a million times the
model's size, made every ray query of KosmicKrisp's structures ten times as dear. Now 27.0 ms, against 20.2 without the
ground (its pixels, most of the frame, shaded). Its BVH is built again with it (UploadStaged). The patch's submesh's
bounds are the patch, the rings' all of them (what the culling goes by); `ModelDesc::bounds` takes in the patch alone
(the shadows' cascades reach as far as it, where the model's shadow falls). `Model::GetFramedBounds` leaves it out,
which the views frame and orbit (`Views::SetScene`'s `framed`, `FrameBounds`' reach for the near plane). `Scene::SceneState::ground` says what the installed model stands on, which View > Scene and
Variant keep when they load it again. A set side by side stands on it (see "Archives of several models"), and a model on a
ground plane is framed from 30 degrees above (`Views::FrameBounds`' pitch), so that a set's back rows show over its front
ones and the shadows are in view.

**The scene's unit** (`gfx::SceneUnit` in `gfx/units.h`: meters, centimeters, millimeters, inches or feet, or a custom
meters per unit; `Scene::Settings::metersPerUnit` holds it, `PushConstants::metersPerUnit` carries it): what the file's
coordinates are in. The lights' inverse square falloff takes distances in meters (`LightFalloff`: candela over meters
squared; the range stays the file's units, as the position is), the ground plane's checker is squares of a meter
(`GroundChecker`: of a unit, a centimeter file's were a centimeter, Sponza's unseen from the camera's 14 m up, the user's
pose `-563.7288,1397.0325,3264.7698,14.237,1.151`, 2026-11-05) and the air goes by it (`AerialPerspectiveData::metersPerUnit`);
what is relative to the model (the framing, the move speed, the clipmap, the cascades, the contacts, the emitters'
importance) doesn't. A file's unit is resolved as its model installs (`ResolveSceneUnit` in `client/ui.cpp`, once per
scene file): the unit chosen for it (`ChooseSceneUnit`: the file browser's "Units" dropdown next to "Ground plane",
`SPEEDO_UNITS=<m|cm|mm|in|ft>` or `SPEEDO_METERS_PER_UNIT=<meters>` with the autoloads, a pasted pose's, which Copy Pose
writes where the unit isn't meters), then remembered for the file; else the file's remembered unit (`RememberedUnit`,
`<user profile>/units.txt`: meters per unit and the scene's file path a line); else guessed (`gfx::GuessUnit`): a gltf's
meters, its spec's; an obj's by its size, centimeters over `kCentimeterExtent` (500) units across (the McGuire archive's
centimeter scenes are 594 to 10860 across: bmw, white oak, holodeck, conference, Sponza, Bistro; its meter ones under
170: San Miguel, Sibenik, salle de bain, erato, Lost Empire, Rungholt's house, the rooms, the small models; teapot, 154,
and the chestnut trees fall to meters), a set's meters (its files are scaled to one size). View > Units shows the unit
and pins one for the scene's file (or a custom Meters per Unit, which was under View > Environment), and its Automatic
forgets the file's, resolved again as a guess. The browser's dropdown's "automatic" (the default) makes no choice: the
remembered or the guessed stands.

Archives of several models are sets of variants (geodesic's 86 polyhedra, sphere's tessellations and texture mappings,
the CornellBox variants, ...), not scenes: each file stands alone at the origin, and the files of a set needn't share a
scale. `Model::Load(filePaths)` loads them as one model, side by side in a grid facing the camera (the xy plane), or on a
ground plane (`ModelGround::kPlane`) laid on it (the xz plane, each file's lowest point on the ground, rows going back),
each scaled to the same size and centered in its cell by its instance transforms (premultiplied by the placement;
vertices are copied as they are), each file still through its own cache entry, merged as staging data before the one
upload (`model::Merge`). What moves keeps moving (`MergeAnimation`): each file's nodes go under a node of its own
placing them, its skins, joints, instance links, morphs, pointer targets and animation channels offset into the set's,
and the set's k-th animation plays every file's k-th; a skinned submesh's instances aren't placed (its joints, under the
placement, place its vertices, which the instance would place again), and the skin vertices are merged as
well. A set has no lights or cameras, nor what animates them. It used to be drawn at rest: a skinned file in its bind
space (CesiumMan lying on its back) and nothing moving. Copy Pose writes a set's files for `SPEEDO_AUTOLOAD_MODEL`,
separated by `;` (its name, a folder's and a count, is no file), and `SPEEDO_AUTOLOAD_ARRANGEMENT=as-authored` where
so, which the autoloads and Paste Pose take. Autoloading a zip or a directory loads all of its models that way, opening a zip offers it next to choosing one,
the browser's Open loads several selected models so (or as authored), and `assettest.ps1` runs the client once per such archive (with the archive's first
image on the default material: sphere.zip's models name materials its mtl file doesn't have), and once per
subdirectory with several models of a directory it is given, rather than once per file: 38 client runs instead of 145
for the McGuire set. The glTF sample models are such subdirectories, of encodings of the same model (glTF, glTF-Binary,
glTF-Embedded, glTF-Quantized, Draco, KTX2, ...): one run each (150 instead of 339), with the encodings the importers
don't support skipped (`mesh::Unsupported`, which only parses the gltf json) rather than failing the set. `assettest`
compares the encodings of each model (files named after the model's directory, in its subdirectories): the same
triangle, vertex, material, submesh and texture counts, and bounds within 1% (quantization rounds). It only warns, since
some differ in the source files: ABeautifulGame's glb is another export (1152 fewer triangles), and StainedGlassLamp's
JPG-PNG variant has 8 materials instead of 13.

**A file's material variants, chosen as it opens** (the Variants popup): a gltf file with variants opened by the user (from
the file browser, File > Open Recent or an archive's chooser: `LoadChoice::kAsk`) asks which to open it with before
anything is loaded (the load posts `gVariantChoice`, as an archive's `gArchiveChoice`; `mesh::VariantNames` reads the
file's json alone): its variants and its default materials, all the variants ticked by default and the default
materials not, often one of them (the user's call); one alone opens the file with it, several lay it out to compare.
The autoloads and Paste Pose never ask (`LoadChoice::kDefault`): `SPEEDO_AUTOLOAD_VARIANT=<index>` opens it with one, and
`SPEEDO_AUTOLOAD_VARIANTS=all`, or a comma separated list of indices and `default` (`ui::ParseVariants`), lays several out:
a model file's, several files', or a folder's or a zip archive's models' (`ui::LoadAutoModel`), each file's that list
names, and a file without any of them once, with its default materials (`ui::LayOutVariants`: a gltf sample model's
encoding without them, as StainedGlassLamp's JPG-PNG), and where no file has any, loads as without it (a set side by
side). Laid out: copies of the files in their scene, each with its variant (`Scene::LoadVariants`, a file and a variant a
copy: a file several times, through `Model::Load`'s per-file `options`, each its own cache entry as variants are), merged
as a set is, one behind the other by default (`ModelArrangement::kBehindEachOther`: each file's copies in a column of side
by side's grid, going back from the camera, the files' columns side by side, the user's call; Merge's groups), or side by
side (the popup's choice, `SPEEDO_AUTOLOAD_ARRANGEMENT=side-by-side`), on the ground plane where the model was. The set
keeps the first file's scenes and variants (View > Scene lays the same copies out in another scene) and records each
copy's variant as asked (`Model::GetLayout`, `Scene::SceneState::layout`, as long as its `filePaths`), which Copy Pose
writes: the files, each once, and the variants of those with any. The views frame a layout whole
(`FrameBounds`' `whole`: its bounding sphere in the field of view; at 1.4 times its radius, a compact model's framing,
the nearest copy was cut off). At first View > Variant chose a variant, its Compare... opened the popup over the model
installed, and each copy was labelled with its variant's name over the views; the popup as the file opens took over
from the menu, and the labels weren't needed (the user's calls).

