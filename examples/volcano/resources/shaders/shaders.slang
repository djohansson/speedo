cbuffer g_view
{
    float4x4 viewProj;
};

Texture2D g_texture;
SamplerState g_sampler;

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
    output.sv_position = mul(viewProj, float4(position, 1.0));

    return output;
}

float4 fragmentMain(CoarseVertex coarseVertex : CoarseVertex) : SV_Target
{
    float3 color = g_texture.Sample(g_sampler, coarseVertex.texCoord).rgb;

    return float4(color, 1.0);
}
