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
#	define UINT(name) uint32_t name
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
#	define UINT(name) uint32_t name
#endif

#define DESCRIPTOR_SET_CATEGORY_GLOBAL 0
#define DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS 1
#define DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES 2
#define DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES 3
#define DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS 4
#define DESCRIPTOR_SET_CATEGORY_VIEW 5
#define DESCRIPTOR_SET_CATEGORY_MATERIAL 6
#define DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES 7

#define SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS 7u
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

struct MaterialData
{
	alignas(16) FLOAT4(color);
	alignas(4) UINT(textureAndSamplerId);
};

struct ModelInstance
{
	alignas(16) FLOAT4X4(modelTransform);
	alignas(16) FLOAT4X4(inverseTransposeModelTransform);
};

struct VertexP3fN3fT014fC4f
{
	alignas(16) FLOAT3(position);
	alignas(16) FLOAT3(normal);
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
};

#ifdef __cplusplus
}
#endif

//NOLINTEND(readability-magic-numbers, modernize-avoid-c-arrays)
