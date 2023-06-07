#pragma once

#if defined(SHADERTYPES_H_CPU_TARGET)
#include <glm/glm.hpp>
#define float4x4 glm::mat4x4
#define float4 glm::vec4
#define float3 glm::vec3
#define float2 glm::vec2
#define uint uint32_t
#else
#define alignas(x)
#endif

#define DescriptorSetCategory_Global                0
#define DescriptorSetCategory_GlobalTextures        1
#define DescriptorSetCategory_GlobalRWTextures      2
#define DescriptorSetCategory_GlobalSamplers        3
#define DescriptorSetCategory_View                  4
#define DescriptorSetCategory_Material              5
#define DescriptorSetCategory_Object                6

#define ShaderTypes_GlobalTextureIndexBits          19u
#define ShaderTypes_GlobalTextureCount              (1u << ShaderTypes_GlobalTextureIndexBits)
#define ShaderTypes_GlobalRWTextureIndexBits        19u
#define ShaderTypes_GlobalRWTextureCount            (1u << ShaderTypes_GlobalTextureIndexBits)
#define ShaderTypes_GlobalSamplerIndexBits          13u
#define ShaderTypes_GlobalSamplerCount              (1u << ShaderTypes_GlobalSamplerIndexBits)
#define ShaderTypes_ViewIndexBits                   16u
#define ShaderTypes_ViewBufferCount                 (1u << ShaderTypes_ViewIndexBits)
#define ShaderTypes_ViewCount                       (1u << (ShaderTypes_ViewIndexBits - 2u)) // frame buffered - needs at least three times as big buffer
#define ShaderTypes_MaterialIndexBits               16u
#define ShaderTypes_MaterialCount                   (1u << ShaderTypes_MaterialIndexBits)
#define ShaderTypes_ObjectBufferIndexBits           16u
#define ShaderTypes_ObjectBufferCount               (1u << ShaderTypes_ObjectBufferIndexBits)
#define ShaderTypes_ObjectBufferInstanceIndexBits   16u
#define ShaderTypes_ObjectBufferInstanceCount       (1u << ShaderTypes_ObjectBufferInstanceIndexBits) // 64 * 65536 = 4MB

// todo: all structs needs to be aligned to VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment.

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

struct ObjectData
{
    alignas(64) float4x4 modelTransform;
    alignas(64) float4x4 inverseTransposeModelTransform;
};

struct PushConstants
{
    alignas(4) uint viewAndMaterialId;
    alignas(4) uint objectBufferAndInstanceId;
};
