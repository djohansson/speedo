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

// the gpu's culling (ComputeCull, see the Cull pass): each of a frame's draws (gCullCandidates[frame]: the draw lists'
// items, list after list), tested against each view that draws its list (gCullJobs[frame], a job per list and view, by
// gCullViews[frame]'s planes). a draw in view takes the next place of its run (the items from runFirst with the same
// pipeline state) in the job's commands (gCullCommands[frame] from commandBase), counted in gCullCounts[frame] from
// countBase, which the run's DrawIndexedIndirectCount reads
#define SHADER_TYPES_CULL_GROUP_SIZE 64u

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
	alignas(4) UINT1(padding0);
	alignas(4) UINT1(padding1);
};

struct CullJob
{
	alignas(4) UINT1(candidateBase); // its list's first item in gCullCandidates
	alignas(4) UINT1(candidateCount); // its list's
	alignas(4) UINT1(view); // in gCullViews
	alignas(4) UINT1(commandBase); // its list's commands (one place per item), in gCullCommands
	alignas(4) UINT1(countBase); // its list's runs' counts, in gCullCounts
	alignas(4) UINT1(padding0);
	alignas(4) UINT1(padding1);
	alignas(4) UINT1(padding2);
};

// a view's frustum (see gfx::Frustum): six planes, xyz the inward normal, w the distance. all zeros culls nothing
struct CullView
{
	alignas(16) FLOAT4(planes[6]);
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
};

#ifdef __cplusplus
}
#endif

//NOLINTEND(readability-magic-numbers, modernize-avoid-c-arrays)
