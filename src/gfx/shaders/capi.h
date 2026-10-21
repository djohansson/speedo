#pragma once

//NOLINTBEGIN(readability-magic-numbers, modernize-avoid-c-arrays)

#ifdef __cplusplus
#if !defined(SHADERTYPES_H_GPU_TARGET)
#	include <cstdint>
#	include <climits>
#endif
extern "C"
{
#else
#if !defined(SHADERTYPES_H_GPU_TARGET)
#	include <stdint.h>
#	include <limits.h>
#endif
#endif

// the scalars are FLOAT1, UINT1 and INT1, not FLOAT, UINT and INT: windows.h writes its typedefs of those names as
// `UINT (CALLBACK *YIELDPROC)(...)`, which a function like macro of the same name rewrites
#if !defined(SHADERTYPES_H_GPU_TARGET)
#	define FLOAT4X4(name) float name[4][4]
#	define FLOAT4_ARRAY(name, count) float name[count][4]
#	define FLOAT4(name) float name[4]
#	define FLOAT3(name) float name[3]
#	define FLOAT2(name) float name[2]
#	define FLOAT1(name) float name
#	define UINT1(name) uint32_t name
#	define UINT2(name) uint32_t name[2]
#	define UINT3(name) uint32_t name[3]
#	define UINT4(name) uint32_t name[4]
#	define INT1(name) int32_t name
#	define INT2(name) int32_t name[2]
#	define INT3(name) int32_t name[3]
#	define INT4(name) int32_t name[4]
#else
#	define alignas(x)
#	define FLT_MAX 3.402823466e+38
#	define FLT_MIN 1.175494351e-38
#	define DBL_MAX 1.7976931348623158e+308
#	define DBL_MIN 2.2250738585072014e-308
#	define FLOAT4X4(name) float4x4 name
#	define FLOAT4_ARRAY(name, count) float4 name[count]
#	define FLOAT4(name) float4 name
#	define FLOAT3(name) float3 name
#	define FLOAT2(name) float2 name
#	define FLOAT1(name) float name
#	define UINT1(name) uint name
#	define UINT2(name) uint2 name
#	define UINT3(name) uint3 name
#	define UINT4(name) uint4 name
#	define INT1(name) int name
#	define INT2(name) int2 name
#	define INT3(name) int3 name
#	define INT4(name) int4 name
#endif

#define DESCRIPTOR_SET_CATEGORY_GLOBAL 0
#define DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS 1
#define DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES 2
#define DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES 3
#define DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS 4
#define DESCRIPTOR_SET_CATEGORY_VIEW 5
#define DESCRIPTOR_SET_CATEGORY_MATERIAL 6
#define DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES 7

#define SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS 10u
#define SHADER_TYPES_GLOBAL_TEXTURE_COUNT (1u << SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS)
#define SHADER_TYPES_GLOBAL_RW_TEXTURE_INDEX_BITS 3u
#define SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT (1u << SHADER_TYPES_GLOBAL_RW_TEXTURE_INDEX_BITS)
#define SHADER_TYPES_GLOBAL_SAMPLER_INDEX_BITS 6u
#define SHADER_TYPES_GLOBAL_SAMPLER_COUNT (1u << SHADER_TYPES_GLOBAL_SAMPLER_INDEX_BITS)
#define SHADER_TYPES_FRAME_INDEX_BITS 2u
#define SHADER_TYPES_FRAME_COUNT (1u << SHADER_TYPES_FRAME_INDEX_BITS)
// gTextures slots: the frames' render targets' color, from this, by frame index
#define SHADER_TYPES_RENDER_TARGET_TEXTURE_BASE 0u
// KHR_materials_volume_scatter: the depth of the scattering materials' nearest back faces, from the Scatter Back pass,
// which the Scatter pass measures their thickness by; the scattering light of their front surfaces (rgb; a: the material
// index plus 1, 0 where there is none), and their depth, from the Scatter pass, which the Scatter Blur pass blurs (see
// SubsurfaceScattering), into the noisy light (the Scatter Blur pass, ComputeScatter), which the Scatter Denoise pass
// (ComputeScatterDenoise) filters into the light the main pass adds: each written as a storage image in gRWTextures
// the multiscatter color per pixel of the Scatter pass (its second color attachment): the albedo the blur's profile is
// scaled by
#define SHADER_TYPES_SCATTER_ALBEDO_TEXTURE 5u
#define SHADER_TYPES_SCATTER_NOISY_TEXTURE 6u
#define SHADER_TYPES_SCATTER_NOISY_RW_TEXTURE 5u
#define SHADER_TYPES_SCATTER_LIGHT_TEXTURE 7u
#define SHADER_TYPES_SCATTER_LIGHT_RW_TEXTURE 4u
#define SHADER_TYPES_SCATTER_BACK_DEPTH_TEXTURE 8u
#define SHADER_TYPES_SCATTER_TEXTURE 9u
#define SHADER_TYPES_SCATTER_DEPTH_TEXTURE 10u
// the gTextures slot of the shadow atlas (see ShadowData)
#define SHADER_TYPES_SHADOW_ATLAS_TEXTURE 11u
// the gTextures slot of the environment's prefiltered panorama (see EnvironmentData)
#define SHADER_TYPES_ENVIRONMENT_TEXTURE 12u
// the gTextures slot of the opaque scene, with mips, which transmissive materials refract (see the main pass's phases)
#define SHADER_TYPES_TRANSMISSION_TEXTURE 13u
// the gTextures slot of the environment's panorama prefiltered for sheen (see EnvironmentData)
#define SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE 14u
// the ui's frosted panels (the menu bar, menus and popups, as macOS's: see UiBlurRect and the UI Blur passes): the
// output under them and the blur's reach around them, which ComputeMain writes, blurred along x (ComputeUiBlurX), then
// along y into the output (ComputeUiBlurY), a gaussian of this radius in pixels
#define SHADER_TYPES_UI_BLUR_SOURCE_TEXTURE 16u
#define SHADER_TYPES_UI_BLUR_SOURCE_RW_TEXTURE 6u
#define SHADER_TYPES_UI_BLUR_ROWS_TEXTURE 17u
#define SHADER_TYPES_UI_BLUR_ROWS_RW_TEXTURE 7u
#define SHADER_TYPES_UI_BLUR_RADIUS 24u
#define SHADER_TYPES_UI_BLUR_RECT_COUNT 32u
#define SHADER_TYPES_UI_BLUR_GROUP_SIZE 64u
// the gSamplers slot of a linear sampler that clamps to the edge (for screen space lookups)
#define SHADER_TYPES_CLAMP_SAMPLER 3u

// order independent transparency: the blended fragments, in a list per pixel (gOitHeads: the first node of each pixel's,
// row by row, PushConstants::framebufferWidth wide; SHADER_TYPES_OIT_NONE ends a list), of nodes in gOitNodes (taken in
// order by gOitCounter[0], up to PushConstants::oitNodeCapacity: the fragments past it are dropped), which ComputeMain
// sorts by depth and blends back to front, at most SHADER_TYPES_OIT_MAX_LAYERS of the nearest per pixel
#define SHADER_TYPES_OIT_NONE 0xffffffffu
#define SHADER_TYPES_OIT_MAX_LAYERS 16u
#define SHADER_TYPES_OIT_NODES_PER_PIXEL 4u

// auto exposure: ComputeMain counts the frame's pixels by their log2 luminance (before exposure) in this many bins of
// gExposureHistogram (per frame index), from SHADER_TYPES_EXPOSURE_LOG2_MIN to SHADER_TYPES_EXPOSURE_LOG2_MAX (the
// first and last bins also take what is below and above), which the cpu reads back to adapt the exposure
#define SHADER_TYPES_EXPOSURE_BINS 64u
#define SHADER_TYPES_EXPOSURE_LOG2_MIN -12.0
#define SHADER_TYPES_EXPOSURE_LOG2_MAX 12.0

// PushConstants::tonemapper
#define TONEMAPPER_PBR_NEUTRAL 0u // Khronos PBR Neutral: base colors render as themselves, brighter ones roll off to white
#define TONEMAPPER_ACES 1u // ACES filmic (Stephen Hill's fit of the RRT and ODT)
#define TONEMAPPER_AGX 2u // AgX (Benjamin Wrensch's polynomial fit, the base look)
#define TONEMAPPER_REINHARD 3u // Reinhard on luminance: x / (1 + x)
#define TONEMAPPER_LINEAR 4u // none: clamped

// PushConstants::debugView (View > Debug View, SPEEDO_DEBUG_VIEW): what the main pass's fragments show instead of their
// light. the surface's values (data: shown as they are, without exposure or tonemapping, see DebugViewIsData)
#define DEBUG_VIEW_NONE 0u
#define DEBUG_VIEW_BASE_COLOR 1u // the base color (linear, texture and vertex colors applied)
#define DEBUG_VIEW_NORMAL 2u // the shading normal (normal mapped), world space, as 0.5 + 0.5 n
#define DEBUG_VIEW_GEOMETRY_NORMAL 3u // the interpolated vertex normal, facing the viewer
#define DEBUG_VIEW_ROUGHNESS 4u
#define DEBUG_VIEW_METALLIC 5u
#define DEBUG_VIEW_OCCLUSION 6u // the material's ambient occlusion
#define DEBUG_VIEW_MATERIAL 7u // a color per material
#define DEBUG_VIEW_TEXCOORD 8u // texcoord set 0, repeated
#define DEBUG_VIEW_SHADOWS 9u // the share of the lights' direct light that isn't shadowed (blue where none reaches)
#define DEBUG_VIEW_LIGHT_COUNT 10u // the lights the fragment loops over (its cluster's), a heat map up to 255
// light (exposed and tonemapped)
#define DEBUG_VIEW_EMISSIVE 11u
#define DEBUG_VIEW_DIRECT 12u // the punctual lights' light alone
#define DEBUG_VIEW_INDIRECT 13u // the ambient light alone: diffuse and specular (the environment's or the indirect lighting's)
#define DEBUG_VIEW_INDIRECT_DIFFUSE 14u // the ambient irradiance over pi, a white surface's (whatever lights it)
#define DEBUG_VIEW_INDIRECT_SPECULAR 15u // the ambient specular alone
// the indirect lighting's (SPEEDO_GI=1): GiData::debug is the view - DEBUG_VIEW_GI_FIRST + 1 (see GiData::debug)
#define DEBUG_VIEW_GI_FIRST 16u
#define DEBUG_VIEW_GI_IRRADIANCE 16u // its indirect diffuse alone, a white surface's (magenta where no clip covers it)
#define DEBUG_VIEW_GI_VISIBILITY 17u // the weight of the probes around a point (how much of them it sees) (data)
#define DEBUG_VIEW_GI_VOXEL_BEHIND 18u // the radiance of the voxel behind the point
#define DEBUG_VIEW_GI_VOXEL_FRONT 19u // and in front of it
#define DEBUG_VIEW_GI_VOXEL_ALBEDO 20u // the voxel's albedo (data)
#define DEBUG_VIEW_GI_REFERENCE 21u // each probe's light from 512 rays straight to the environment (seconds a frame)
#define DEBUG_VIEW_GI_CLIP 22u // which clip lights the point (red, green, blue, yellow: finest first), blended (data)
#define DEBUG_VIEW_GI_GRID 23u // the indirect diffuse, with the voxels' (thin) and probes' (thick) cells of the clip drawn
// the voxels themselves, as the rays see them: each pixel's ray from the eye through the clipmap's voxels (finest
// first, see GiTraceVoxels), and the first it meets (the environment where it meets none): its radiance, its albedo, its
// normal (data), or which clip it is in (data)
#define DEBUG_VIEW_GI_VOXEL_WORLD 24u
#define DEBUG_VIEW_GI_VOXEL_WORLD_ALBEDO 25u
#define DEBUG_VIEW_GI_VOXEL_WORLD_NORMAL 26u
// the irradiance of the 8 probes around a point, trilinear, as the probes hold it: without their visibility, the
// resolve into the voxels and the volumes (to tell what the probes see from what the lookups make of it)
#define DEBUG_VIEW_GI_PROBES 27u
// the irradiance at the point itself: 256 cosine weighted rays from it through the voxels (seconds a frame), what the
// probes' lookups approximate there
#define DEBUG_VIEW_GI_SURFACE_REFERENCE 28u
// the state of the 8 probes around a point, trilinear (data): red the share marked inside something, green the share
// whose depth map toward the point is empty (no ray near that way yet), blue the share that fail the visibility test
#define DEBUG_VIEW_GI_PROBE_STATE 29u
#define DEBUG_VIEW_COUNT 30u
#define SHADER_TYPES_VIEW_INDEX_BITS 4u
#define SHADER_TYPES_VIEW_COUNT (1u << SHADER_TYPES_VIEW_INDEX_BITS)
// gMaterialData's size to begin with: the installed model's materials are 1 and up (0 is the default material), and it
// grows to hold them (see Scene's GrowMaterials)
#define SHADER_TYPES_MATERIAL_COUNT 1024u

// caution: don't change the alignment unless you know what you are doing.
struct ViewData
{
	alignas(16) FLOAT4X4(viewProjection);
	alignas(16) FLOAT4(eyePosition); // xyz: the camera's position, in world space (for the specular lighting)
	alignas(16) FLOAT4X4(inverseViewProjection); // for the environment backdrop's rays (see ComputeMain)
	alignas(16) FLOAT4(viewport); // x, y, width, height: where it is drawn in the render target, in pixels
};

// how many lights gLights holds: a model's, then the environment's dominant lights (ThousandsOfLights.gltf has 4000)
#define SHADER_TYPES_LIGHT_COUNT 4096u

// shadows: each shadowed light's views into the shadow atlas (one depth texture, SHADER_TYPES_SHADOW_ATLAS_TEXTURE), per
// frame: gShadowViews[frame], from the light's first, gLightShadows[frame][light] (SHADER_TYPES_NO_SHADOW for a light
// without). a directional light has SHADER_TYPES_SHADOW_CASCADE_COUNT views (cascades, nearest first), a point light 6
// (the cube's faces: +x, -x, +y, -y, +z, -z), a spot light 1
#define SHADER_TYPES_SHADOW_VIEW_COUNT 256u
#define SHADER_TYPES_SHADOW_CASCADE_COUNT 4u
#define SHADER_TYPES_NO_SHADOW 0xffffffffu
#define SHADER_TYPES_NOT_SKINNED 0xffffffffu

#define LIGHT_TYPE_DIRECTIONAL 0u
#define LIGHT_TYPE_POINT 1u
#define LIGHT_TYPE_SPOT 2u

// a punctual light (gltf KHR_lights_punctual), in world space. gLights holds PushConstants::lightCount of them.
// the environment the scene is lit by (image based lighting, see gfx/environment.h): gEnvironment[0]
struct EnvironmentData
{
	// the irradiance divided by pi, as spherical harmonics (bands 0 to 2, rgb): what a white Lambertian surface reflects
	alignas(16) FLOAT4_ARRAY(irradiance, 9);
	// the prefiltered panorama (equirectangular, +y up, -z at its center): mip i for roughness i / (levelCount - 1)
	alignas(4) UINT1(textureId);
	alignas(4) UINT1(samplerId);
	alignas(4) FLOAT1(levelCount);
	// the same for KHR_materials_sheen's Charlie lobe, in SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE (sampled with samplerId)
	alignas(4) FLOAT1(sheenLevelCount);
	// the procedural sky (gfx::environment::SkyParameters), which the shader evaluates where it is seen sharply (the
	// backdrop, mirror reflections) rather than sampling the panorama's few texels: rgb of its colors, and skyZenith.w 1
	// if the environment is the procedural sky (else 0, and none of these are used). skySun: xyz towards the sun (in the
	// panorama's space), w the cosine of its radius; skySunRadiance.x its disk's radiance
	alignas(16) FLOAT4(skyZenith);
	alignas(16) FLOAT4(skyHorizon);
	alignas(16) FLOAT4(skyGround);
	alignas(16) FLOAT4(skySun);
	alignas(16) FLOAT4(skySunRadiance);
};

// the environment's prefiltering kernels (the Environment layout, environment.slang): the roughness of the level written,
// and the source's mip count (its pyramid, down to 8 texels wide)
struct EnvironmentFilterConstants
{
	alignas(4) FLOAT1(roughness);
	alignas(4) UINT1(sourceLevelCount);
	alignas(4) UINT1(padding0);
	alignas(4) UINT1(padding1);
};

struct OitNode
{
	alignas(4) UINT1(color); // rgb times alpha, as R11G11B10 floats (r in the low bits)
	alignas(4) UINT1(alphaDepth); // alpha as 8 bit unorm in the high bits, depth (0 near, 1 far) as 24 bit unorm below
	alignas(4) UINT1(next); // the pixel's next node, or SHADER_TYPES_OIT_NONE
};

struct LightData
{
	alignas(16) FLOAT4(positionRange); // xyz: where it is (point, spot). w: its range, 0 for none (inverse square falloff)
	alignas(16) FLOAT4(direction); // xyz: where it shines (directional, spot), unit length
	// rgb: its color times its intensity, in lux (directional) or candela (point, spot). linear
	alignas(16) FLOAT4(intensity);
	// spot: the cone's attenuation, saturate(dot(direction, -l) * x + y)^2. type: LIGHT_TYPE_*
	alignas(4) FLOAT1(spotScale);
	alignas(4) FLOAT1(spotOffset);
	alignas(4) UINT1(type);
	alignas(4) UINT1(padding);
};

// a view a light's shadow is drawn from: world to clip space (depth 0 to 1), drawn in a tile of the atlas
struct ShadowData
{
	alignas(16) FLOAT4X4(viewProjection);
	alignas(16) FLOAT4(atlasRect); // xy: the tile's corner in the atlas, zw: its size (texture coordinates)
	// x: a texel's size in world units (orthographic), or per unit of distance from the light (perspective). y: 1 for a
	// perspective view, else 0. zw: unused
	alignas(16) FLOAT4(params);
};

// clustered lighting: each view's frustum is a grid of clusters, SHADER_TYPES_CLUSTER_TILES_X by _Y tiles of the screen
// (by normalized device coordinates) and SHADER_TYPES_CLUSTER_SLICES slices of depth (logarithmic, see ClusterView), each
// with the lights that may reach it: gClusters[frame][view * SHADER_TYPES_CLUSTER_COUNT + cluster] is its first index
// into gClusterLights[frame] and its count, planned on the cpu each frame (see gfx/clusters.h). a fragment loops over
// its cluster's lights only, or over all of gLights where its view has no clusters (ClusterView::enabled 0)
#define SHADER_TYPES_CLUSTER_TILES_X 16u
#define SHADER_TYPES_CLUSTER_TILES_Y 9u
#define SHADER_TYPES_CLUSTER_SLICES 24u
#define SHADER_TYPES_CLUSTER_COUNT (SHADER_TYPES_CLUSTER_TILES_X * SHADER_TYPES_CLUSTER_TILES_Y * SHADER_TYPES_CLUSTER_SLICES)
// the light indices gClusterLights holds per frame (past them, the views loop over all lights)
#define SHADER_TYPES_CLUSTER_LIGHT_INDEX_COUNT (1u << 18)

// a frosted panel of the ui (gUiBlurRects[frame], PushConstants::uiBlurRectCount of them): where the output's backdrop is
// blurred, a rectangle with rounded corners in pixels of the output. the UI Blur passes run a thread per pixel of each
// (ComputeUiBlurX over it and the blur's reach above and below, ComputeUiBlurY over it), from its first thread
struct UiBlurRect
{
	alignas(16) FLOAT4(rect); // xy: its top left corner, zw: its bottom right (exclusive), in pixels
	alignas(4) FLOAT1(radius); // its corners' radius, in pixels
	alignas(4) UINT1(firstThreadX); // ascending with the panels: each panel's threads follow the one's before
	alignas(4) UINT1(firstThreadY);
	alignas(4) FLOAT1(opacity); // how much of the blur shows (the ui fading, see ImGuiLayer::Fade)
};

// how a view's clusters cut its frustum (gClusterViews[frame][view])
struct ClusterView
{
	// xyz: the view's forward direction (unit length), w: so that dot(xyz, p) + w is a point's distance from the near
	// plane, which is sliced as floor(log2(1 + distance * depthInverseUnit) * depthScale)
	alignas(16) FLOAT4(depthPlane);
	alignas(4) FLOAT1(depthInverseUnit);
	alignas(4) FLOAT1(depthScale);
	alignas(4) UINT1(enabled); // 0: no clusters (the view loops over all lights)
	alignas(4) UINT1(padding);
};

#define SHADER_TYPES_TEXTURE_VIEW_INDEX_BITS 12u
#define SHADER_TYPES_TEXTURE_VIEW_COUNT (1u << SHADER_TYPES_TEXTURE_VIEW_INDEX_BITS)

// a texture as a material samples it (see gfx::TextureRef): gTextures[textureId] with gSamplers[samplerId], at its
// texcoord set (texCoord01.xy or .zw) transformed: (u', v') = (dot(uTransform.xyz, (u, v, 1)), dot(vTransform.xyz, (u, v, 1)))
// a normal map whose y points up the image (a KTX2 file's, kept as it is: see gfx::image::Image::normalYUp), which the
// shader flips to the renderer's convention (y down the image, as sampled)
#define TEXTURE_VIEW_FLAG_NORMAL_Y_UP 1u

struct TextureView
{
	alignas(16) FLOAT4(uTransform);
	alignas(16) FLOAT4(vTransform);
	alignas(4) UINT1(textureId);
	alignas(4) UINT1(samplerId);
	alignas(4) UINT1(texCoordSet); // 0 or 1
	alignas(4) UINT1(flags); // TEXTURE_VIEW_FLAG_*
};

// which of a material's textures it has, each sampled through its view (an index into gTextureViews)
// layer groups a graphics pipeline can be specialized without (the shaders' specialization constant 0, kSkippedLayers):
// their code compiles away. the extended layers: clearcoat, sheen, transmission (and volume and dispersion), anisotropy,
// iridescence and diffuse transmission
#define SHADER_TYPES_LAYERS_EXTENDED 1u

#define MATERIAL_FLAG_TEXTURE 1u // baseColorView: an srgb color (and alpha), multiplied in, and alpha tested against alphaCutoff
#define MATERIAL_FLAG_ALPHA_TEXTURE 2u // alphaView: a mask in r, tested against alphaCutoff
#define MATERIAL_FLAG_NORMAL_TEXTURE 4u // normalView: a tangent space normal map, x and y in rg
#define MATERIAL_FLAG_EMISSIVE_TEXTURE 8u // emissiveView: an srgb color, times emissive
#define MATERIAL_FLAG_OCCLUSION_TEXTURE 16u // occlusionView: ambient occlusion in r, by emissive.w
#define MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE 32u // metallicRoughnessView: roughness in r, metallic in g, times the factors
#define MATERIAL_FLAG_UNLIT 64u // gltf KHR_materials_unlit: drawn in its base color, without lighting
#define MATERIAL_FLAG_SPECULAR_TEXTURE 128u // specularView: the specular strength in r, times specular
#define MATERIAL_FLAG_SPECULAR_COLOR_TEXTURE 256u // specularColorView: an srgb color times specularColor (and glossiness in a)
#define MATERIAL_FLAG_SPECULAR_GLOSSINESS 512u // specular-glossiness: specularColor is f0, roughness the glossiness
#define MATERIAL_FLAG_CLEARCOAT 1024u // a clearcoat layer (see clearcoat)
#define MATERIAL_FLAG_CLEARCOAT_TEXTURE 2048u // clearcoatView: the strength in r, times clearcoat.x
#define MATERIAL_FLAG_CLEARCOAT_ROUGHNESS_TEXTURE 4096u // clearcoatRoughnessView: the roughness in r, times clearcoat.y
#define MATERIAL_FLAG_CLEARCOAT_NORMAL_TEXTURE 8192u // clearcoatNormalView: the layer's normal map, by clearcoat.z
#define MATERIAL_FLAG_SHEEN 16384u // a sheen lobe (see sheen)
#define MATERIAL_FLAG_SHEEN_COLOR_TEXTURE 32768u // sheenColorView: an srgb color times sheen.rgb
#define MATERIAL_FLAG_SHEEN_ROUGHNESS_TEXTURE 65536u // sheenRoughnessView: the roughness in r, times sheen.a
#define MATERIAL_FLAG_TRANSMISSION 131072u // transmission (see transmission)
#define MATERIAL_FLAG_TRANSMISSION_TEXTURE 262144u // transmissionView: the factor in r, times transmission.x
#define MATERIAL_FLAG_THICKNESS_TEXTURE 524288u // thicknessView: the thickness in r, times transmission.y
#define MATERIAL_FLAG_ANISOTROPY 1048576u // anisotropy (see anisotropy)
#define MATERIAL_FLAG_ANISOTROPY_TEXTURE 2097152u // anisotropyView: the direction in rg (0 to 1), the strength in b
#define MATERIAL_FLAG_IRIDESCENCE 4194304u // iridescence (see iridescence)
#define MATERIAL_FLAG_IRIDESCENCE_TEXTURE 8388608u // iridescenceView: the factor in r, times iridescence.x
#define MATERIAL_FLAG_IRIDESCENCE_THICKNESS_TEXTURE 16777216u // iridescenceThicknessView: in r, from iridescence.z to .w
#define MATERIAL_FLAG_DIFFUSE_TRANSMISSION 33554432u // diffuse transmission (see diffuseTransmission)
#define MATERIAL_FLAG_DIFFUSE_TRANSMISSION_TEXTURE 67108864u // diffuseTransmissionView: the factor in r, times .a
#define MATERIAL_FLAG_DIFFUSE_TRANSMISSION_COLOR_TEXTURE 134217728u // diffuseTransmissionColorView: srgb, times .rgb
#define MATERIAL_FLAG_RETROREFLECTION 268435456u // retroreflection (see retroreflection)
#define MATERIAL_FLAG_RETROREFLECTION_TEXTURE 536870912u // retroreflectionView: the weight in r, times retroreflection
#define MATERIAL_FLAG_VOLUME_SCATTER 1073741824u // subsurface scattering (see multiscatterColor)
#define MATERIAL_FLAG_VOLUME_SCATTER_TEXTURE 2147483648u // multiscatterColorView: an srgb color times multiscatterColor.rgb

struct MaterialData
{
	alignas(16) FLOAT4(color);
	// rgb: linear light added after lighting (may be above 1), times the emissive texture. a: the occlusion strength
	alignas(16) FLOAT4(emissive);
	// rgb: the dielectric specular color (see gfx::mesh::Material::specularColor), or with
	// MATERIAL_FLAG_SPECULAR_GLOSSINESS the color at normal incidence itself. a: the index of refraction
	alignas(16) FLOAT4(specularColor);
	// KHR_materials_clearcoat: x the strength, y the roughness, z the normal map's scale
	alignas(16) FLOAT4(clearcoat);
	// KHR_materials_sheen: rgb the color, a the roughness
	alignas(16) FLOAT4(sheen);
	// KHR_materials_transmission, volume and dispersion: x the transmission, y the thickness, z the attenuation
	// distance (0: none), w the dispersion
	alignas(16) FLOAT4(transmission);
	alignas(16) FLOAT4(attenuationColor); // rgb
	// KHR_materials_anisotropy: x the strength, y and z the cosine and sine of the rotation
	alignas(16) FLOAT4(anisotropy);
	// KHR_materials_iridescence: x the factor, y the film's ior, z and w the thickness range (nanometers)
	alignas(16) FLOAT4(iridescence);
	// KHR_materials_diffuse_transmission: rgb the color, a the factor
	alignas(16) FLOAT4(diffuseTransmission);
	alignas(4) UINT1(flags);
	alignas(4) FLOAT1(alphaCutoff); // fragments with a lower texture alpha are discarded: 0 for opaque materials
	alignas(4) FLOAT1(normalScale); // scales the normal map's x and y (gltf normalTexture.scale)
	alignas(4) UINT1(baseColorView);
	alignas(4) UINT1(alphaView);
	alignas(4) UINT1(normalView);
	alignas(4) UINT1(emissiveView);
	alignas(4) UINT1(occlusionView);
	// the glTF metallic-roughness model: 0 to 1 each (perceptual roughness, squared for the brdf). 1 and 0 for obj
	// materials, which are matte. with MATERIAL_FLAG_SPECULAR_GLOSSINESS, roughness is the glossiness
	alignas(4) FLOAT1(metallic);
	alignas(4) FLOAT1(roughness);
	alignas(4) UINT1(metallicRoughnessView);
	alignas(4) FLOAT1(specular); // the dielectric specular's strength (see mesh::Material::specular): 1 by default
	alignas(4) UINT1(specularView);
	alignas(4) UINT1(specularColorView);
	alignas(4) UINT1(clearcoatView);
	alignas(4) UINT1(clearcoatRoughnessView);
	alignas(4) UINT1(clearcoatNormalView);
	alignas(4) UINT1(sheenColorView);
	alignas(4) UINT1(sheenRoughnessView);
	alignas(4) UINT1(transmissionView);
	alignas(4) UINT1(thicknessView);
	alignas(4) UINT1(anisotropyView);
	alignas(4) UINT1(iridescenceView);
	alignas(4) UINT1(iridescenceThicknessView);
	alignas(4) UINT1(diffuseTransmissionView);
	alignas(4) UINT1(diffuseTransmissionColorView);
	// KHR_materials_retroreflection: the blend from the specular lobe to its retroreflective one (see Shade)
	alignas(4) FLOAT1(retroreflection);
	alignas(4) UINT1(retroreflectionView);
	// KHR_materials_volume_scatter: rgb the multiple scattering albedo (with the volume's attenuation, which is the
	// extinction, see SingleScatterAlbedo)
	alignas(16) FLOAT4(multiscatterColor);
	alignas(4) UINT1(multiscatterColorView);
};

struct ModelInstance
{
	alignas(16) FLOAT4X4(modelTransform);
	alignas(16) FLOAT4X4(inverseTransposeModelTransform);
};

struct VertexP3fN3fTa4fT014fC4f
{
	alignas(16) FLOAT3(position);
	alignas(16) FLOAT3(normal);
	// xyz: along +u of the normal map's texcoords (before its transform). w: the handedness of the frame (gltf's), whose
	// bitangent cross(normal, tangent.xyz) * w points up the image, i.e. along -v. w = 0: no tangent, the fragment shader
	// derives the frame from screen space derivatives
	alignas(16) FLOAT4(tangent);
	alignas(16) FLOAT4(texCoord01);
	alignas(16) FLOAT4(color);
};

// a skinned vertex's joints and weights (gltf JOINTS_0 and WEIGHTS_0), by vertex index (gSkinVertices, parallel to
// gVertexBuffer): position = sum of weight i * gJointMatrices[frame][DrawRecord::jointBase + joint i] * position
struct SkinVertex
{
	alignas(8) UINT2(joints); // 4 x 16 bit joint indices into its skin, low half first
	alignas(8) UINT2(weights); // 4 x 16 bit unorm weights, summing to 1
};

// a vertex's deltas for one morph target, transformed like the vertex: gMorphDeltas holds a row of
// DrawRecord::morphTargetCount of them per vertex of an animated morph submesh, from morphDeltaBase for its vertices
// from morphFirstVertex, added to the vertex by the target's weight (gMorphWeights[frame][morphWeightBase + target])
struct MorphDelta
{
	alignas(16) FLOAT4(position);
	alignas(16) FLOAT4(normal);
	alignas(16) FLOAT4(tangent);
};

// how many draws a frame's gDrawRecords holds to begin with (the draw lists' items, see DrawList): it grows to hold them
#define SHADER_TYPES_DRAW_RECORD_COUNT (1u << 15)

// what a draw needs that differs from the draw before (gDrawRecords[frame][first instance]: each draw, direct or
// indirect, names its record by its first instance, SV_StartInstanceLocation, which SV_InstanceID doesn't count): its
// material, its instances, and its skin and morph targets. read by the vertex stage, which hands the material on
struct DrawRecord
{
	alignas(4) UINT1(materialId); // gMaterialData's
	alignas(4) UINT1(modelInstanceId); // the first of its instances in gModelInstances, offset by SV_InstanceID
	alignas(4) UINT1(jointBase); // where its skin's joint matrices start in gJointMatrices, or SHADER_TYPES_NOT_SKINNED
	// its morph targets (see MorphDelta), none if morphTargetCount is 0
	alignas(4) UINT1(morphTargetCount);
	alignas(4) UINT1(morphDeltaBase);
	alignas(4) UINT1(morphFirstVertex);
	alignas(4) UINT1(morphWeightBase);
	alignas(4) UINT1(padding);
};

// an indexed indirect draw's command (VkDrawIndexedIndirectCommand's layout)
struct DrawIndexedIndirectCommand
{
	alignas(4) UINT1(indexCount);
	alignas(4) UINT1(instanceCount);
	alignas(4) UINT1(firstIndex);
	alignas(4) INT1(vertexOffset);
	alignas(4) UINT1(firstInstance);
};

// the gpu's culling (ComputeCull, see the Cull passes): each of a frame's draws (gCullCandidates[frame]: the draw lists'
// items, list after list), tested against each view that draws its list (gCullJobs[frame], a job per list and view, by
// gCullViews[frame]'s planes, and by the depth pyramid). a draw it keeps takes the next place of its run (the items from
// runFirst with the same pipeline state) in the job's commands (gCullCommands[frame] from commandBase), counted in
// gCullCounts[frame] from countBase, which the run's DrawIndexedIndirectCount reads
#define SHADER_TYPES_CULL_GROUP_SIZE 64u
// what a job keeps of its list's draws in view (CullJob::kind): all of them; those seen last frame (gCullVisibility),
// the main pass's first draws, which the depth pyramid is made of; those the pyramid doesn't hide that weren't drawn
// first (and whether each is seen, for the next frame); those the pyramid doesn't hide (the transmissive phase's)
#define CULL_JOB_FRUSTUM 0u
#define CULL_JOB_EARLY 1u
#define CULL_JOB_LATE 2u
#define CULL_JOB_PYRAMID 3u
// with the depth reprojected from the last frame (see ComputeDepthReproject) as the pyramid's: those it doesn't hide,
// drawn first (and marked so in gCullVisibility), then of the rest, those the pyramid of what was drawn doesn't hide
#define CULL_JOB_HYBRID_EARLY 4u
#define CULL_JOB_HYBRID_LATE 5u
// what the occlusion culling decided in a frame, per draw and view (gCullStatistics[frame], a group of these per
// CullJob::statistics: the main views', then the shadow views'): drawn before the pyramid (or in one phase), opaque and
// blended draws drawn after it, and the draws in view it hid; and of every culling job (frustum only too), the draws
// outside the view's frustum (counted by the job that decides last: once per draw and view) and the triangles of the
// draws kept (times their instances)
#define CULL_STATISTIC_EARLY 0u
#define CULL_STATISTIC_LATE 1u
#define CULL_STATISTIC_LATE_BLENDED 2u
#define CULL_STATISTIC_HIDDEN 3u
#define CULL_STATISTIC_FRUSTUM 4u
#define CULL_STATISTIC_TRIANGLES 5u
#define CULL_STATISTIC_COUNT 6u
#define CULL_STATISTIC_MAIN 0u
#define CULL_STATISTIC_SHADOW 1u
#define CULL_STATISTIC_GROUP_COUNT 2u
// the depth pyramid (ComputeDepthPyramid, see the Depth Pyramid pass): the main depth's farthest per texel, from the
// main depth at level 0 (a copy) up to 1x1, each level a storage image of gDepthPyramid, which occlusion reads
#define SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT 16u
// and the shadow atlas's (gShadowDepthPyramid: half the atlas at level 0), which the shadow views' occlusion reads. the
// atlas's tiles are power of two squares aligned to their size, so a level whose texels are no bigger than a tile reads
// that tile alone (CullView::maxLevel)
#define SHADER_TYPES_SHADOW_DEPTH_PYRAMID_LEVEL_COUNT 12u
#define SHADER_TYPES_DEPTH_PYRAMID_GROUP_SIZE 8u
// the gTextures slot of the main pass's depth (the depth pyramid's source)
#define SHADER_TYPES_MAIN_DEPTH_TEXTURE 4u
// the depth reprojection: a thread per pixel, as the pyramid's
#define SHADER_TYPES_DEPTH_REPROJECT_GROUP_SIZE 8u

struct CullCandidate
{
	alignas(16) FLOAT4(boundsMin); // xyz: its bounds' corner (world space). w: 1 if it may be culled (see DrawItem)
	alignas(16) FLOAT4(boundsMax);
	alignas(4) UINT1(record); // its DrawRecord: its command's first instance
	alignas(4) UINT1(indexCount);
	alignas(4) UINT1(instanceCount);
	alignas(4) UINT1(firstIndex);
	alignas(4) UINT1(run); // its run, of its list's
	alignas(4) UINT1(runFirst); // its run's first item, in its list
	alignas(4) UINT1(late); // 1: never drawn first (blended: drawn after every opaque draw, see FragmentTransparent)
	alignas(4) UINT1(padding0);
};

struct CullJob
{
	alignas(4) UINT1(candidateBase); // its list's first item in gCullCandidates
	alignas(4) UINT1(candidateCount); // its list's
	alignas(4) UINT1(view); // in gCullViews
	alignas(4) UINT1(commandBase); // its list's commands (one place per item), in gCullCommands
	alignas(4) UINT1(countBase); // its list's runs' counts, in gCullCounts
	alignas(4) UINT1(kind); // CULL_JOB_*
	alignas(4) UINT1(visibilityBase); // early and late: its list's draws' visibility for the view, in gCullVisibility
	// 1: the draws kept compacted per run and counted (DrawIndexedIndirectCount). 0: every draw's command in its own place,
	// of no instances if not kept (DrawIndexedIndirect of the whole run: where the count is emulated, see
	// DeviceLimits::drawIndirectCountEmulated)
	alignas(4) UINT1(compact);
	alignas(4) UINT1(statistics); // its decisions' group in gCullStatistics (CULL_STATISTIC_MAIN or _SHADOW)
	alignas(4) UINT1(padding0);
	alignas(4) UINT1(padding1);
	alignas(4) UINT1(padding2);
};

// a view's frustum (see gfx::Frustum): six planes, xyz the inward normal, w the distance (all zeros culls nothing), and,
// for occlusion, its view projection and viewport (in framebuffer pixels: x, y, width, height)
struct CullView
{
	alignas(16) FLOAT4(planes[6]);
	alignas(16) FLOAT4X4(viewProjection);
	alignas(16) FLOAT4(viewport);
	alignas(4) UINT1(occlusion); // whether the depth pyramid hides draws from it
	// whether the last frame saved its depth (gPreviousDepth) with this view, and its inverse view projection and viewport
	// then (the depth reprojection's)
	alignas(4) UINT1(previousValid);
	// the pyramid it is occluded by (0: gDepthPyramid, the main views'; 1: gShadowDepthPyramid, a shadow view's, in its
	// tile of the atlas, its viewport), and the coarsest level whose texels stay in its viewport
	alignas(4) UINT1(pyramid);
	alignas(4) UINT1(maxLevel);
	alignas(16) FLOAT4X4(previousInverseViewProjection);
	alignas(16) FLOAT4(previousViewport);
};

// indirect lighting (see gi.slang and gfx/indirectlight.h): world space irradiance probes over a voxel radiance volume.
// the volume: a grid of voxels over the scene (GiData::volume*), voxelized from what doesn't move (gGiVoxels: albedo and
// occupancy, normal, emission per voxel), lit each frame into radiance and occupancy (gGiRadiance, a mip chain of levels
// each half the one below). the probes: a lattice over the volume (GiData::probe*), each tracing GiData::rayCount rays
// a frame through the radiance (a different subset of a fixed set every frame, see GiData::rayPhase), blended into what it holds
// (GiData::hysteresis): its irradiance as spherical harmonics (gGiIrradiance) and the distance to what it sees per
// direction (gGiProbeDepth: an octahedral map of the mean and the mean square, for a visibility test), which the shading
// interpolates, each probe weighed by whether it sees the point (see ComputeGiResolve)
#define SHADER_TYPES_GI_GROUP_SIZE 64u
// the kernels' threads are dispatched in rows of this many (a dispatch has at most 65535 groups a dimension)
#define SHADER_TYPES_GI_ROW 4096u
// the lights that light the voxels, binned into a grid of cells over the volume (gGiLightCells: a first index and count
// into gGiLightIndices per cell)
#define SHADER_TYPES_GI_LIGHT_GRID 16u
// the clipmap: volumes of the same voxel counts, each of voxels twice the size of the one before, the finest around the
// camera and the last over the scene (gGi[frame][clip]); a clip's rays and shadow rays that leave it go on through the
// coarser ones, and the shading takes the finest that holds a point
#define SHADER_TYPES_GI_CLIP_COUNT 4u
// the irradiance's spherical harmonics coefficients (bands 0 to 2) per probe and color
#define SHADER_TYPES_GI_SH_COUNT 9u
// the voxels' resolved probes (gGiVolumes): per color its coefficients 0 to 3 and 4 to 7, then the colors' coefficient 8
// and the weight
#define SHADER_TYPES_GI_VOLUME_COUNT 7u
// a probe's depth map: an octahedral map of this many texels a side (gGiProbeDepth, DDGI's 16), each the mean distance
// to what the probe sees that way and the mean of its square (half2), in probe spacings, as far as
// SHADER_TYPES_GI_DEPTH_REACH spacings (the points it is tested for are within its cell: see ComputeGiResolve); a texel
// takes the rays near its direction by cos^SHADER_TYPES_GI_DEPTH_SHARPNESS of their angle to it (DDGI's 50: at 8x8 and
// 10 a texel toward a wall mixed the wall's distance with what is beyond it, and a sunlit probe outside the wall lit the
// points at its base inside). a probe's update handles SHADER_TYPES_GI_DEPTH_TEXELS_PER_THREAD texels a thread
#define SHADER_TYPES_GI_DEPTH_SIDE 16u
#define SHADER_TYPES_GI_DEPTH_SHARPNESS 50.0
#define SHADER_TYPES_GI_DEPTH_TEXELS_PER_THREAD 4u
#define SHADER_TYPES_GI_DEPTH_REACH 3.0f
// the flags (gGiFlags, see ComputeGiFlags): per voxel of this level of each clip (a block) SHADER_TYPES_GI_FLAG_WORD_COUNT
// words: in the first, bits 8 and up the last frame (GiData::frameStamp) anything was voxelized in it, bit 0 whether
// anything is in the 3x3x3 blocks around it, and bit 3 whether bit 0 is set within SHADER_TYPES_GI_FLAG_REACH blocks
// along each axis (bits 1 and 2 are the spread's steps along x and y); in the second, the last frame something moving
// was voxelized in it (see ComputeGiLight)
#define SHADER_TYPES_GI_FLAG_LEVEL 2u
#define SHADER_TYPES_GI_FLAG_REACH 4
#define SHADER_TYPES_GI_FLAG_STAMP_SHIFT 8u
#define SHADER_TYPES_GI_FLAG_WORD_COUNT 2u
// what the probes' update counts (gGiStatistics[frame], host visible): the probes traced this frame, which the next
// frame's rays per probe are budgeted by (see Renderer::InternalPlanGi)
#define GI_STATISTIC_ACTIVE_PROBES 0u
// diagnostic: rays whose walk hit its step cap (see GiMarch), and the last one's origin and direction
#define GI_STATISTIC_CAPPED_RAYS 1u
#define GI_STATISTIC_CAPPED_ORIGIN 2u
#define GI_STATISTIC_CAPPED_DIRECTION 5u
#define GI_STATISTIC_CAPPED_STATE 8u // clip, t, endHere, boxExit, voxel xyz, travelled
#define GI_STATISTIC_COUNT 16u
// a probe dumped for debugging (SPEEDO_GI_DUMP=<x,y,z>: per clip the probe nearest the point writes its state after
// gGiStatistics[frame]'s statistics, GI_DUMP_FLOAT_COUNT floats per clip: its position (3), offset (3), age, inside, the
// rays that met a surface's back, the rays, then its depth map's texels' means)
#define GI_DUMP_FLOAT_COUNT 316u

struct GiVoxel
{
	alignas(4) UINT1(albedo); // rgba8: the diffuse albedo, linear, and 255 in a where something is
	alignas(4) UINT1(normal); // two snorm16: the normal, octahedral
	alignas(4) UINT1(emissionRG); // half2
	alignas(4) UINT1(emissionB); // half; bits 16 and 17 which faces of a triangle wrote it (both: a thin wall's voxel)
};

struct GiData
{
	// the volume: its corner (world space) and its voxels' size, and its voxels per axis (w unused)
	alignas(16) FLOAT4(volumeOrigin);
	alignas(16) UINT4(volumeSize);
	// when it is voxelized (see ComputeGiShift): how far its voxels moved since the frame before, in voxels (a voxel
	// takes the one that was at its place plus this; the others, which scrolled in, are voxelized). its size, or more,
	// to voxelize all of it. the probes move with them (by whole probes: the clips move by whole probe cells)
	alignas(16) INT4(scroll);
	// the probes: the corner their lattice is laid out from (xyz, world space: the clip's) and their spacing (w, world
	// space); their count per axis, and their spacing in voxels (w)
	alignas(16) FLOAT4(probeOrigin);
	alignas(16) UINT4(probeSize);
	// a clip that follows the eye: the box around the eye (xyz its center, world space) that it always holds well inside
	// wherever it snapped to, of half its size per axis (fadeExtent.xyz), its weight fading to 0 over the outermost
	// fadeExtent.w of it (see GiClipWeight). by the eye, which moves smoothly, rather than by the clip's snapped edges:
	// fading over 8 voxels from them, narrower than a snap (16), points switched from one clip to the next at once as it
	// snapped, and the clips' lighting differs (a coarser clip's voxels are thicker, its lookups further off the surface:
	// Sponza's inside by 15 to 25 of 255), so whole regions lit up or darkened. the last clip: a box holding everything
	alignas(16) FLOAT4(fadeCenter);
	alignas(16) FLOAT4(fadeExtent);
	alignas(4) UINT1(enabled); // whether the shading takes its diffuse indirect light from the probes
	alignas(4) UINT1(lightCount); // of gLights, which light the voxels
	// the debug view (PushConstants::debugView from DEBUG_VIEW_GI_FIRST, + 1; SPEEDO_GI_DEBUG): 1 shades with the
	// indirect diffuse light alone (a white surface's), 2 with the probes' visibility (the weight of the probes around a
	// point, see ComputeGiResolve), 3 with the radiance of the voxel behind the point (4: in front of it), 5 with its
	// albedo (magenta where there is none, cyan outside the volume), 6 the probes' reference (see ComputeGiProbeUpdate),
	// 7 the clip a point is lit by, 8 the indirect diffuse with the clip's voxel and probe cells drawn
	alignas(4) UINT1(debug);
	// the clip's first voxel in gGiVoxels and first cell in gGiLightCells; the clipmap's clip count, and this clip's index
	// (the coarser ones follow it in gGi)
	alignas(4) UINT1(voxelFirst);
	alignas(4) UINT1(lightCellFirst);
	alignas(4) UINT1(clipCount);
	alignas(4) UINT1(clip);
	// the clip's first voxel in gGiRadiance (the frame's radiance per voxel, what the rays see: ComputeGiLight), and where
	// after them the voxels' direct radiance is kept: the lights' light off the static voxels,
	// made when the clip is voxelized or the lights change (ComputeGiLightStatic), which the frame's lighting adds the
	// bounce to (ComputeGiLight: a moving voxel's is made in the frame)
	alignas(4) UINT1(radianceFirst);
	alignas(4) UINT1(directFirst);
	// the clip's first flag in gGiFlags: per voxel of level 2 (a 4x4x4 block), whether anything is voxelized near it
	// (see ComputeGiFlags)
	alignas(4) UINT1(flagFirst);
	// the probes' buffers hold two copies of every clip's probes (halves), the frame's and the last frame's, which it
	// reads (shifted by scroll) and blends its rays into: the clip's first probe in gGiIrradiance (SHADER_TYPES_GI_SH_COUNT
	// float4 each) in this frame's half and in the last frame's, its first texel in gGiProbeDepth in each, and its first
	// ray in gGiRays (rayCount per probe)
	alignas(4) UINT1(probeFirst);
	alignas(4) UINT1(probePreviousFirst);
	alignas(4) UINT1(depthFirst);
	alignas(4) UINT1(depthPreviousFirst);
	alignas(4) UINT1(rayFirst);
	alignas(4) UINT1(rayCount);
	// the frame's rays are a set of rayCount directions (a Fibonacci sphere) turned by the frame's phase (rayPhase, the
	// next each frame, of rayPhaseCount; see GiRayDirection), the same for every probe: over rayPhaseCount frames a probe
	// traces the same sets each time around, so that what it holds converges rather than wanders (a set turned at random
	// every frame, as DDGI's, wanders by the frame's rays' share)
	alignas(4) UINT1(rayPhase);
	alignas(4) UINT1(rayPhaseCount);
	// how much of what a probe held it keeps a frame (the rest is this frame's rays'); a probe new to the clip takes the
	// coarser clip's light where it is, and more of its own rays until it has as many frames as that
	alignas(4) FLOAT1(hysteresis);
	// whether nothing is kept from the last frame (the scene changed, or the buffers were replaced): every probe starts
	// from its rays, and the voxels are lit by the lights alone
	alignas(4) UINT1(reset);
	// whether the voxels' radiance adds what their albedo reflects of the probes' light at them (the last frame's: the
	// light bounces on, a bounce a frame; SPEEDO_GI_BOUNCE=0 for the lights' light alone)
	alignas(4) UINT1(bounce);
	// the frame, counted from 1 (in 24 bits): what the blocks holding something are stamped with (see gGiFlags)
	alignas(4) UINT1(frameStamp);
	// whether a probe blends faster through a change of the light far beyond its noise (see ComputeGiProbeUpdate;
	// SPEEDO_GI_ADAPT=1: off by default, see Renderer)
	alignas(4) UINT1(adapt);
	// whether anything moving is voxelized this frame (a static voxel's kept light is made again where a moving block
	// lies between it and a light, see ComputeGiLight)
	alignas(4) UINT1(moving);
	// the probe of this clip dumped for debugging (its index among the clip's, or ~0: see GI_DUMP_FLOAT_COUNT)
	alignas(4) UINT1(dumpProbe);
};

struct PushConstants
{
	// per frame
	alignas(4) UINT1(frameIndex);
	// per view: gViewData's
	alignas(4) UINT1(viewId);
	// per frame: how many of gLights light the scene, and the exposure the final image is scaled by before tonemapping
	alignas(4) UINT1(lightCount);
	alignas(4) FLOAT1(exposure);
	// per frame: the environment's (see EnvironmentData) radiance scale, and its rotation about +y in radians (positive
	// turns it counterclockwise seen from above)
	alignas(4) FLOAT1(environmentIntensity);
	alignas(4) FLOAT1(environmentRotation);
	// per frame, for ComputeMain: how many views gViewData holds, and whether the environment is drawn where nothing
	// else is (the backdrop)
	alignas(4) UINT1(viewCount);
	alignas(4) UINT1(environmentBackdrop);
	// per frame: the order independent transparency's (see OitNode)
	alignas(4) UINT1(framebufferWidth);
	alignas(4) UINT1(oitNodeCapacity);
	// per frame, for ComputeMain: which tonemapper (TONEMAPPER_*)
	alignas(4) UINT1(tonemapper);
	// per draw: the shadow view (gShadowViews[frame]) a draw of the shadow pass is seen from, else SHADER_TYPES_NO_SHADOW
	// (the view of viewId). per frame: whether lights cast their shadows
	alignas(4) UINT1(shadowView);
	alignas(4) UINT1(shadows);
	// per frame: whether the scatter pass drew (SHADER_TYPES_SCATTER_TEXTURE holds this frame's scattering light)
	alignas(4) UINT1(scatter);
	// per frame: how many of gUiBlurRects[frame] the ui's frosted panels take (0: none)
	alignas(4) UINT1(uiBlurRectCount);
	// per dispatch: the first of gCullJobs[frame] a Cull pass runs, and the level the Depth Pyramid pass writes
	alignas(4) UINT1(cullJobBase);
	alignas(4) UINT1(depthPyramidLevel);
	// per dispatch: what the Depth Pyramid's level 0 reduces (0: the main depth, 1: gReprojectedDepth), and the frame's
	// height; the last frame's extent (gPreviousDepth's), and its main view count (the depth reprojection's)
	alignas(4) UINT1(depthPyramidSource);
	alignas(4) UINT1(framebufferHeight);
	alignas(4) UINT1(previousWidth);
	alignas(4) UINT1(previousHeight);
	alignas(4) UINT1(mainViewCount);
	// per dispatch: the indirect lighting's level, and its clip (see GiData)
	alignas(4) UINT1(giStep);
	alignas(4) UINT1(giClip);
	// per frame: what the main pass's fragments show (DEBUG_VIEW_*: their light, or a debug view of the surface or the
	// lighting), and so how ComputeMain maps it (see DebugViewIsData)
	alignas(4) UINT1(debugView);
};

#ifdef __cplusplus
}
#endif

//NOLINTEND(readability-magic-numbers, modernize-avoid-c-arrays)
