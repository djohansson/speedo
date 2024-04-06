#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(SHADERTYPES_H_CPU_TARGET)
#	include <cstdint>
#	include <glm/glm.hpp>
#	define float4x4 glm::mat4x4
#	define float4 glm::vec4
#	define float3 glm::vec3
#	define float2 glm::vec2
#	define uint uint32_t
#else
#	define alignas(x)
#endif

#define DescriptorSetCategory_Global 0
#define DescriptorSetCategory_GlobalBuffers 1
#define DescriptorSetCategory_GlobalTextures 2
#define DescriptorSetCategory_GlobalRWTextures 3
#define DescriptorSetCategory_GlobalSamplers 4
#define DescriptorSetCategory_View 5
#define DescriptorSetCategory_Material 6
#define DescriptorSetCategory_ModelInstances 7

#define ShaderTypes_GlobalTextureIndexBits		7u
#define ShaderTypes_GlobalTextureCount			(1u << ShaderTypes_GlobalTextureIndexBits)
#define ShaderTypes_GlobalRWTextureIndexBits	3u
#define ShaderTypes_GlobalRWTextureCount		(1u << ShaderTypes_GlobalRWTextureIndexBits)
#define ShaderTypes_GlobalSamplerIndexBits		4u
#define ShaderTypes_GlobalSamplerCount			(1u << ShaderTypes_GlobalSamplerIndexBits)
#define ShaderTypes_FrameIndexBits				2u
#define ShaderTypes_FrameCount					(1u << ShaderTypes_FrameIndexBits)
#define ShaderTypes_ViewIndexBits				4u
#define ShaderTypes_ViewCount					(1u << ShaderTypes_ViewIndexBits)
#define ShaderTypes_MaterialIndexBits			10u
#define ShaderTypes_MaterialCount				(1u << ShaderTypes_MaterialIndexBits)
#define ShaderTypes_ModelInstanceIndexBits		19u
#define ShaderTypes_ModelInstanceCount			(1u << ShaderTypes_ModelInstanceIndexBits)

// todo: all structs needs to be aligned to VkPhysicalDeviceLimits.min(Uniform|Structured)BufferOffsetAlignment.

struct ViewData
{
	alignas(64) float4x4 viewProjectionTransform;
	alignas(16) float3 eyePosition;
};

struct MaterialData
{
	alignas(16) float4 color;
	alignas(4) uint textureAndSamplerId;
};

struct ModelInstance
{
	alignas(64) float4x4 modelTransform;
	alignas(64) float4x4 inverseTransposeModelTransform;
};

struct Vertex_P3f_N3f_T03f_C03f
{
	alignas(16) float3 position;
	alignas(16) float3 normal;
	alignas(8) float2 texCoord;
	alignas(16) float3 color;
};

struct PushConstants
{
	// per frame
	alignas(4) uint frameIndex;
	// per view
	// per material
	alignas(4) uint viewAndMaterialId;
};

#ifdef __cplusplus
}
#endif
