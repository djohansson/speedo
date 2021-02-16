#pragma once

#if !defined(GPU)
#define GPU false
#include <glm/glm.hpp>
#define float4x4 glm::mat4x4
#define float4 glm::vec4
#define uint uint32_t
#else
#define alignas(x)
#endif

#define DescriptorSetCategory_Global                0
#define DescriptorSetCategory_GlobalTextures        1
#define DescriptorSetCategory_GlobalSamplers        2

#define ShaderTypes_ViewIndexBits 16
#define ShaderTypes_ViewBufferCount (1 << ShaderTypes_ViewIndexBits)
#define ShaderTypes_ViewCount (1 << (ShaderTypes_ViewIndexBits - 2)) // frame buffered - needs at least three times as big buffer
#define ShaderTypes_MaterialIndexBits 16
#define ShaderTypes_MaterialCount (1 << ShaderTypes_MaterialIndexBits)
#define ShaderTypes_ObjectBufferIndexBits 16
#define ShaderTypes_ObjectBufferCount (1 << ShaderTypes_ObjectBufferIndexBits)
#define ShaderTypes_ObjectBufferInstanceIndexBits 16
#define ShaderTypes_ObjectBufferInstanceCount (1 << ShaderTypes_ObjectBufferInstanceIndexBits) // 64 * 65536 = 4MB. goal is for whole buffer to fit in one large page.
#define ShaderTypes_TextureIndexBits 20
#define ShaderTypes_TextureCount (1u << ShaderTypes_TextureIndexBits)
#define ShaderTypes_SamplerIndexBits 12
#define ShaderTypes_SamplerCount (1u << ShaderTypes_SamplerIndexBits)

// todo: all structs needs to be aligned to VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment.

struct ViewData
{
    alignas(64) float4x4 viewProjectionMatrix;
};

struct MaterialData
{
    alignas(16) float4 color;
    alignas(4) uint textureAndSamplerId;
};

struct ObjectData
{
    alignas(64) float4x4 localTransform;
};

struct PushConstants
{
    alignas(4) uint viewAndMaterialId;
    alignas(4) uint objectBufferAndInstanceId;
};
