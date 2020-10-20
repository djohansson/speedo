struct ViewParams
{
    float4x4 viewProj;
};

ConstantBuffer<ViewParams> g_view;

struct MaterialParams
{
	Texture2D texture;
    SamplerState sampler;
};

[[vk::binding(0, 2)]]
[[vk::push_constant]]
ParameterBlock<MaterialParams> g_materialParams;

struct InputStream
{
    float3 position : POSITION;
    float3 color : COLOR;
    float2 texCoord : TEXCOORD0;
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
    
    float3 color = g_materialParams.texture.Sample(g_materialParams.sampler, input.texCoord).rgb;

    //output.color = float4(color + g_materialParams.color, 1.0);
    output.color = float4(color, 1.0);

    return output;
}

//RWStructuredBuffer<float4> colorTarget;
//RWTexture2D<float4> colorTarget;

//[numthreads(1, 1, 1)]
//void computeMain(uint3 threadIds : SV_DispatchThreadID)
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

//[[vk::push_constant]]
//ParameterBlock<MaterialParams> g_pushDescriptor;
