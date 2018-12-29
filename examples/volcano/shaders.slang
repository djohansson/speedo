cbuffer Uniforms
{
    float4x4 model;
    float4x4 view;
    float4x4 proj;
};

Texture2D<float4> g_texture;

SamplerState TextureSampler
{
    Filter = MIN_MAG_MIP_LINEAR;
    AddressU = Wrap;
    AddressV = Wrap;
};

struct AssembledVertex
{
    float3 position : POSITION;
    float3 color : COLOR;
    float2 texCoord : TEXCOORD0;
};

struct CoarseVertex
{
    float3 color;
    float2 texCoord;
};

struct Fragment
{
    float4 color;
};

struct VertexStageOutput
{
    CoarseVertex coarseVertex : CoarseVertex;
    float4 sv_position : SV_Position;
};

VertexStageOutput vertexMain(AssembledVertex assembledVertex)
{
    VertexStageOutput output;

    float3 position = assembledVertex.position;
    float3 color = assembledVertex.color;
    float2 texCoord = assembledVertex.texCoord;

    output.coarseVertex.color = color;
    output.coarseVertex.texCoord = texCoord;
    output.sv_position = mul(proj, mul(view, mul(model, float4(position, 1.0))));

    return output;
}

float4 fragmentMain(CoarseVertex coarseVertex : CoarseVertex) : SV_Target
{
    //float3 color = coarseVertex.color;
    float3 color = g_texture.Sample(TextureSampler, coarseVertex.texCoord);

    return float4(color, 1.0);
}
