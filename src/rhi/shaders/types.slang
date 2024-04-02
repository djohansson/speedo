implementing speedo;

interface IInputStream
{
}

struct InputStream : IInputStream
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float2 texCoord : TEXCOORD0;
	float3 color : COLOR;
	uint vertexId : SV_VertexID;
};

interface IInterpolants
{
}

struct Interpolants : IInterpolants
{
	float3 position;
	float3 normal;
	float2 texCoord;
	float3 color;
};

struct VertexOutput
{
	float4 hPos : SV_Position;
	Interpolants data : Interpolants;
};

struct FragmentOutput
{
	float4 color : SV_Target;
};