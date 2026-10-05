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
#define SHADER_TYPES_GLOBAL_SAMPLER_INDEX_BITS 4u
#define SHADER_TYPES_GLOBAL_SAMPLER_COUNT (1u << SHADER_TYPES_GLOBAL_SAMPLER_INDEX_BITS)
#define SHADER_TYPES_FRAME_INDEX_BITS 2u
#define SHADER_TYPES_FRAME_COUNT (1u << SHADER_TYPES_FRAME_INDEX_BITS)
#define SHADER_TYPES_VIEW_INDEX_BITS 4u
#define SHADER_TYPES_VIEW_COUNT (1u << SHADER_TYPES_VIEW_INDEX_BITS)
#define SHADER_TYPES_MATERIAL_INDEX_BITS 10u
#define SHADER_TYPES_MATERIAL_COUNT (1u << SHADER_TYPES_MATERIAL_INDEX_BITS)
#define SHADER_TYPES_MODEL_INSTANCE_INDEX_BITS 19u
#define SHADER_TYPES_MODEL_INSTANCE_COUNT (1u << SHADER_TYPES_MODEL_INSTANCE_INDEX_BITS)

// caution: don't change the alignment unless you know what you are doing.
struct ViewData
{
	alignas(16) FLOAT4X4(viewProjection);
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

struct MaterialData
{
	alignas(16) FLOAT4(color);
	// rgb: linear light added after lighting (may be above 1), times the emissive texture. a: the occlusion strength
	alignas(16) FLOAT4(emissive);
	alignas(4) UINT(flags);
	alignas(4) FLOAT(alphaCutoff); // fragments with a lower texture alpha are discarded: 0 for opaque materials
	alignas(4) FLOAT(normalScale); // scales the normal map's x and y (gltf normalTexture.scale)
	alignas(4) UINT(baseColorView);
	alignas(4) UINT(alphaView);
	alignas(4) UINT(normalView);
	alignas(4) UINT(emissiveView);
	alignas(4) UINT(occlusionView);
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

struct PushConstants
{
	// per frame
	alignas(4) UINT(frameIndex);
	// per view
	// per material
	alignas(4) UINT(viewAndMaterialId);
	// per draw
	alignas(4) UINT(modelInstanceId);
};

#ifdef __cplusplus
}
#endif

//NOLINTEND(readability-magic-numbers, modernize-avoid-c-arrays)
