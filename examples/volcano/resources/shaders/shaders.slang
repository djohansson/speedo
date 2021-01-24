#include "shadertypes.h"

// [[vk::constant_id(1)]] const bool  specConstBool  = true;
// [[vk::constant_id(2)]] const int   specConstInt   = 42;
// [[vk::constant_id(3)]] const float specConstFloat = 1.5;

struct ViewParams
{
    float4x4 viewProj;
};

[[vk::binding(0, DescriptorSetCategory_Global)]]
ConstantBuffer<ViewParams> g_view;

//[[vk::binding(0, DescriptorSetCategory_View), vk::push_constant]]
// ViewParams g_view;

struct MaterialParams
{
    float4 color;
	Texture2D texture;
    SamplerState sampler;
};

[[vk::binding(0, DescriptorSetCategory_Material)]]
ParameterBlock<MaterialParams> g_material;

struct ObjectParams
{
    float4 color;
	Texture2D texture;
    SamplerState sampler;
};

[[vk::binding(0, DescriptorSetCategory_Object)]]
ParameterBlock<ObjectParams> g_object;

struct InputStream
{
    float3 position : POSITION;
    float3 color : COLOR;
    float2 texCoord : TEXCOORD0;
    uint vertexId : SV_VertexID;
};

struct Interpolants
{
    float3 color;
    float2 texCoord;
};

struct VSOutput
{
    Interpolants data : Interpolants;
    float4 hPos : SV_Position;
};

VSOutput vertexMain(InputStream input)
{
    VSOutput output;

    float3 position = input.position;
    float3 color = input.color;
    float2 texCoord = input.texCoord;

    output.data.color = color;
    output.data.texCoord = texCoord;
    output.hPos = mul(g_view.viewProj, float4(position, 1.0));

    return output;
}

struct FSOutput
{
    float4 color : SV_Target;
};

FSOutput fragmentMain(Interpolants input : Interpolants)
{
    FSOutput output;
    
    float4 color = (float4)0;//float4(input.color, 1.0);

    color += 0.25 * g_material.texture.Sample(g_material.sampler, input.texCoord);
    color += 0.25 * g_material.color;
    color += 0.25 * g_object.texture.Sample(g_object.sampler, input.texCoord);
    color += 0.25 * g_object.color;

    output.color = color;
    
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
