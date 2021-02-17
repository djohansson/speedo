#include "shadertypes.h"

[[vk::binding(0, DescriptorSetCategory_Global)]]
[[vk::push_constant]]
ConstantBuffer<PushConstants> g_pushConstants;

[[vk::binding(1, DescriptorSetCategory_Global)]]
StructuredBuffer<ViewData> g_viewData;

[[vk::binding(2, DescriptorSetCategory_Global)]]
StructuredBuffer<MaterialData> g_materialData;

[[vk::binding(3, DescriptorSetCategory_Global)]]
StructuredBuffer<ObjectData> g_objectData[ShaderTypes_ObjectBufferCount];

[[vk::binding(0, DescriptorSetCategory_GlobalTextures)]]
Texture2D g_textures[ShaderTypes_TextureCount];

[[vk::binding(0, DescriptorSetCategory_GlobalSamplers)]]
SamplerState g_samplers[ShaderTypes_SamplerCount];

struct InputStream
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD0;
    float3 color : COLOR;
    uint vertexId : SV_VertexID;
};

struct Interpolants
{
    float3 position;
    float3 normal;
    float2 texCoord;
    float3 color;
};

struct VSOutput
{
    float4 hPos : SV_Position;
    Interpolants data : Interpolants;
};

struct FSOutput
{
    float4 color : SV_Target;
};

VSOutput vertexMain(InputStream input)
{
    VSOutput output = (VSOutput)0;

    uint viewAndMaterialId = g_pushConstants.viewAndMaterialId;

    uint viewId = viewAndMaterialId >> ShaderTypes_MaterialIndexBits;
    
    ViewData view = g_viewData[viewId];

    uint objectBufferAndInstanceId = g_pushConstants.objectBufferAndInstanceId;

    uint objectBufferIndex = objectBufferAndInstanceId >> ShaderTypes_ObjectBufferInstanceIndexBits;
    uint objectInstanceIndex = objectBufferAndInstanceId & (ShaderTypes_ObjectBufferInstanceCount - 1);

    ObjectData object = g_objectData[objectInstanceIndex][objectBufferIndex];

    output.hPos = mul(mul(view.viewProjectionMatrix, object.localTransform), float4(input.position, 1.0));
    output.data.position = input.position;
    output.data.normal = input.normal;
    output.data.texCoord = input.texCoord;
    output.data.color = input.color;

    return output;
}

FSOutput fragmentMain(Interpolants input : Interpolants)
{
    FSOutput output = (FSOutput)0;

    uint viewAndMaterialId = g_pushConstants.viewAndMaterialId;

    uint materialId = viewAndMaterialId & (ShaderTypes_MaterialCount - 1);
    
    MaterialData material = g_materialData[materialId];

    uint textureAndSamplerId = material.textureAndSamplerId;
    
    uint textureId = textureAndSamplerId >> ShaderTypes_TextureIndexBits;
    uint samplerId = textureAndSamplerId & (ShaderTypes_SamplerCount - 1);

    Texture2D texture = g_textures[textureId];
    SamplerState sampler = g_samplers[samplerId];
    
    output.color += 0.25 * float4(input.color, 1.0);
    output.color += 0.5 * texture.Sample(sampler, input.texCoord);
    output.color += 0.25 * material.color;
    
    return output;
}

//RWStructuredBuffer<float4> colorTarget;
//RWTexture2D<float4> colorTarget;

//[numthreads(1, 1, 1)]
//void computeMain(uint3 threadIds : SV_DispatchThreadID, uint3 groupId : SV_GroupID)
//{
    // float4 color[3];
    // color[0].r = (r0 >> 0  & 0xFF) / 255.0f;
    // color[0].g = (r0 >> 8  & 0xFF) / 255.0f;
    // color[0].b = (r0 >> 16 & 0xFF) / 255.0f;
    // color[0].a = (r0 >> 24 & 0xFF) / 255.0f;

    // color[1].r = (r1 >> 0  & 0xFF) / 255.0f;
    // color[1].g = (r1 >> 8  & 0xFF) / 255.0f;
    // color[1].b = (r1 >> 16 & 0xFF) / 255.0f;
    // color[1].a = (r1 >> 24 & 0xFF) / 255.0f;

    // color[2].r = (r2 >> 0  & 0xFF) / 255.0f;
    // color[2].g = (r2 >> 8  & 0xFF) / 255.0f;
    // color[2].b = (r2 >> 16 & 0xFF) / 255.0f;
    // color[2].a = (r2 >> 24 & 0xFF) / 255.0f;

    // float4 result = lerp(lerp(lerp(0, color[2], color[2].a), color[1], color[1].a), color[0], color[0].a);
    // result.a = 1.0f;
    // colorTarget[threadIds.xy] = result;
//}
