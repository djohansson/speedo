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

#define ShaderTypes_MaxViews 16
#define ShaderTypes_MaxMaterials 256
#define ShaderTypes_MaxObjectBuffers 256
#define ShaderTypes_MaxObjectBufferInstances 65535 // 64 * 65536 = 4MB. goal is for whole buffer to fit in one large page.
#define ShaderTypes_MaxTextures 1048576
#define ShaderTypes_MaxSamplers 65536


// todo: all structs needs to be aligned to VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment.

struct ViewData
{
    alignas(64) float4x4 viewProjectionMatrix;
};

struct MaterialData
{
    alignas(16) float4 color;
    alignas(4) uint textureId;
    alignas(4) uint samplerId;
};

struct ObjectData
{
    alignas(64) float4x4 localTransform;
};

struct PushConstants
{
    alignas(4) uint viewId;
    alignas(4) uint objectId;
    alignas(4) uint materialId;
};
