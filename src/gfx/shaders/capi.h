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

#if !defined(SHADERTYPES_H_GPU_TARGET)
#	define FLOAT4X4(name) float name[4][4]
#	define FLOAT4_ARRAY(name, count) float name[count][4]
#	define FLOAT4(name) float name[4]
#	define FLOAT3(name) float name[3]
#	define FLOAT2(name) float name[2]
#	define FLOAT(name) float name
#	define UINT(name) uint32_t name
#	define UINT2(name) uint32_t name[2]
#	define UINT3(name) uint32_t name[3]
#	define UINT4(name) uint32_t name[4]
#	define INT(name) int32_t name
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
#	define FLOAT(name) float name
#	define UINT(name) uint name
#	define UINT2(name) uint2 name
#	define UINT3(name) uint3 name
#	define UINT4(name) uint4 name
#	define INT(name) int name
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
// the gTextures slot of the environment's prefiltered panorama (see EnvironmentData)
#define SHADER_TYPES_ENVIRONMENT_TEXTURE 12u
// the gTextures slot of the opaque scene, with mips, which transmissive materials refract (see the main pass's phases)
#define SHADER_TYPES_TRANSMISSION_TEXTURE 13u
// the gSamplers slot of a linear sampler that clamps to the edge (for screen space lookups)
#define SHADER_TYPES_CLAMP_SAMPLER 3u

// order independent transparency: the blended fragments, in a list per pixel (gOitHeads: the first node of each pixel's,
// row by row, PushConstants::framebufferWidth wide; SHADER_TYPES_OIT_NONE ends a list), of nodes in gOitNodes (taken in
// order by gOitCounter[0], up to PushConstants::oitNodeCapacity: the fragments past it are dropped), which ComputeMain
// sorts by depth and blends back to front, at most SHADER_TYPES_OIT_MAX_LAYERS of the nearest per pixel
#define SHADER_TYPES_OIT_NONE 0xffffffffu
#define SHADER_TYPES_OIT_MAX_LAYERS 16u
#define SHADER_TYPES_OIT_NODES_PER_PIXEL 4u
#define SHADER_TYPES_VIEW_INDEX_BITS 4u
#define SHADER_TYPES_VIEW_COUNT (1u << SHADER_TYPES_VIEW_INDEX_BITS)
#define SHADER_TYPES_MATERIAL_INDEX_BITS 10u
#define SHADER_TYPES_MATERIAL_COUNT (1u << SHADER_TYPES_MATERIAL_INDEX_BITS)

// caution: don't change the alignment unless you know what you are doing.
struct ViewData
{
	alignas(16) FLOAT4X4(viewProjection);
	alignas(16) FLOAT4(eyePosition); // xyz: the camera's position, in world space (for the specular lighting)
	alignas(16) FLOAT4X4(inverseViewProjection); // for the environment backdrop's rays (see ComputeMain)
	alignas(16) FLOAT4(viewport); // x, y, width, height: where it is drawn in the render target, in pixels
};

#define SHADER_TYPES_LIGHT_COUNT 256u
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
	alignas(4) UINT(textureId);
	alignas(4) UINT(samplerId);
	alignas(4) FLOAT(levelCount);
	alignas(4) FLOAT(padding);
};

struct OitNode
{
	alignas(4) UINT(color); // rgb times alpha, as R11G11B10 floats (r in the low bits)
	alignas(4) UINT(alphaDepth); // alpha as 8 bit unorm in the high bits, depth (0 near, 1 far) as 24 bit unorm below
	alignas(4) UINT(next); // the pixel's next node, or SHADER_TYPES_OIT_NONE
};

struct LightData
{
	alignas(16) FLOAT4(positionRange); // xyz: where it is (point, spot). w: its range, 0 for none (inverse square falloff)
	alignas(16) FLOAT4(direction); // xyz: where it shines (directional, spot), unit length
	// rgb: its color times its intensity, in lux (directional) or candela (point, spot). linear
	alignas(16) FLOAT4(intensity);
	// spot: the cone's attenuation, saturate(dot(direction, -l) * x + y)^2. type: LIGHT_TYPE_*
	alignas(4) FLOAT(spotScale);
	alignas(4) FLOAT(spotOffset);
	alignas(4) UINT(type);
	alignas(4) UINT(padding);
};

#define SHADER_TYPES_TEXTURE_VIEW_INDEX_BITS 12u
#define SHADER_TYPES_TEXTURE_VIEW_COUNT (1u << SHADER_TYPES_TEXTURE_VIEW_INDEX_BITS)

// a texture as a material samples it (see gfx::TextureRef): gTextures[textureId] with gSamplers[samplerId], at its
// texcoord set (texCoord01.xy or .zw) transformed: (u', v') = (dot(uTransform.xyz, (u, v, 1)), dot(vTransform.xyz, (u, v, 1)))
struct TextureView
{
	alignas(16) FLOAT4(uTransform);
	alignas(16) FLOAT4(vTransform);
	alignas(4) UINT(textureId);
	alignas(4) UINT(samplerId);
	alignas(4) UINT(texCoordSet); // 0 or 1
	alignas(4) UINT(padding);
};

// which of a material's textures it has, each sampled through its view (an index into gTextureViews)
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
	alignas(4) UINT(flags);
	alignas(4) FLOAT(alphaCutoff); // fragments with a lower texture alpha are discarded: 0 for opaque materials
	alignas(4) FLOAT(normalScale); // scales the normal map's x and y (gltf normalTexture.scale)
	alignas(4) UINT(baseColorView);
	alignas(4) UINT(alphaView);
	alignas(4) UINT(normalView);
	alignas(4) UINT(emissiveView);
	alignas(4) UINT(occlusionView);
	// the glTF metallic-roughness model: 0 to 1 each (perceptual roughness, squared for the brdf). 1 and 0 for obj
	// materials, which are matte. with MATERIAL_FLAG_SPECULAR_GLOSSINESS, roughness is the glossiness
	alignas(4) FLOAT(metallic);
	alignas(4) FLOAT(roughness);
	alignas(4) UINT(metallicRoughnessView);
	alignas(4) FLOAT(specular); // the dielectric specular's strength (see mesh::Material::specular): 1 by default
	alignas(4) UINT(specularView);
	alignas(4) UINT(specularColorView);
	alignas(4) UINT(clearcoatView);
	alignas(4) UINT(clearcoatRoughnessView);
	alignas(4) UINT(clearcoatNormalView);
	alignas(4) UINT(sheenColorView);
	alignas(4) UINT(sheenRoughnessView);
	alignas(4) UINT(transmissionView);
	alignas(4) UINT(thicknessView);
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
// gVertexBuffer): position = sum of weight i * gJointMatrices[frame][PushConstants::jointBase + joint i] * position
struct SkinVertex
{
	alignas(8) UINT2(joints); // 4 x 16 bit joint indices into its skin, low half first
	alignas(8) UINT2(weights); // 4 x 16 bit unorm weights, summing to 1
};

// a vertex's deltas for one morph target, transformed like the vertex: gMorphDeltas holds a row of
// PushConstants::morphTargetCount of them per vertex of an animated morph submesh, from morphDeltaBase for its vertices
// from morphFirstVertex, added to the vertex by the target's weight (gMorphWeights[frame][morphWeightBase + target])
struct MorphDelta
{
	alignas(16) FLOAT4(position);
	alignas(16) FLOAT4(normal);
	alignas(16) FLOAT4(tangent);
};

struct PushConstants
{
	// per frame
	alignas(4) UINT(frameIndex);
	// per view
	// per material
	alignas(4) UINT(viewAndMaterialId);
	// per draw: the first of its instances in gModelInstances (the model's instance buffer), offset by SV_InstanceID
	alignas(4) UINT(modelInstanceId);
	// per frame: how many of gLights light the scene, and the exposure the final image is scaled by before tonemapping
	alignas(4) UINT(lightCount);
	alignas(4) FLOAT(exposure);
	// per draw: where its skin's joint matrices start in gJointMatrices, or SHADER_TYPES_NOT_SKINNED
	alignas(4) UINT(jointBase);
	// per draw: its morph targets (see MorphDelta), none if morphTargetCount is 0
	alignas(4) UINT(morphTargetCount);
	alignas(4) UINT(morphDeltaBase);
	alignas(4) UINT(morphFirstVertex);
	alignas(4) UINT(morphWeightBase);
	// per frame: the environment's (see EnvironmentData) radiance scale, and its rotation about +y in radians (positive
	// turns it counterclockwise seen from above)
	alignas(4) FLOAT(environmentIntensity);
	alignas(4) FLOAT(environmentRotation);
	// per frame, for ComputeMain: how many views gViewData holds, and whether the environment is drawn where nothing
	// else is (the backdrop)
	alignas(4) UINT(viewCount);
	alignas(4) UINT(environmentBackdrop);
	// per frame: the order independent transparency's (see OitNode)
	alignas(4) UINT(framebufferWidth);
	alignas(4) UINT(oitNodeCapacity);
};

#ifdef __cplusplus
}
#endif

//NOLINTEND(readability-magic-numbers, modernize-avoid-c-arrays)
