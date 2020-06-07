cbuffer g_view
{
    float4x4 viewProj;
};

Texture2D g_texture;
SamplerState g_sampler;

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
    output.hPos = mul(viewProj, float4(position, 1.0));

    return output;
}

struct FSOutput
{
    float4 color : SV_Target;
};

FSOutput fragmentMain(Interpolants input : Interpolants)
{
    FSOutput output;
    
    float3 color = g_texture.Sample(g_sampler, input.texCoord).rgb;

    output.color = float4(color, 1.0);

    return output;
}

RWStructuredBuffer<float4> colorTarget;

[numthreads(1, 1, 1)]
void computeMain(uint3 threadIds : SV_DispatchThreadID)
{
}
