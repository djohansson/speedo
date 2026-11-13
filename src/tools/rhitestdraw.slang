// rhitest's draw (tools/rhitest.cpp): a triangle over the target's upper left half (as Vulkan's clip space has it, y
// down), shaded by where it is (its interpolated coordinates times the draw's color), at the draw's depth, offset and
// scaled by the draw: asymmetric, so that a backend drawing it mirrored or wound the other way differs from Vulkan's.
// the draw's constants are the push constants (VertexRhiTestDraw), or for an indirect draw its record in gDraws, named by
// its first instance as the client's draws name theirs (VertexRhiTestDrawIndirect, SV_StartInstanceLocation)

struct RhiTestDrawConstants
{
	float4 color;
	float2 offset;
	float2 scale; // per axis: a negative one mirrors the triangle, winding it the other way
	float depth;
	// to 48 bytes, explicitly: Metal's buffers are packed (slang's natural layout, which Metal's target always takes),
	// Vulkan's std430 rounds the struct up to its float4's 16 bytes, so gDraws' records were 36 bytes apart on Metal and
	// 48 on Vulkan before (scalars: a float3 would align to 16 in std430)
	float padding0;
	float padding1;
	float padding2;
};

[[vk::binding(0, 0)]]
[[vk::push_constant]]
ConstantBuffer<RhiTestDrawConstants> gRhiTestDraw;

struct RhiTestDrawSet
{
	StructuredBuffer<RhiTestDrawConstants> gDraws;
};
[[vk::binding(0, 1)]]
ParameterBlock<RhiTestDrawSet> gSetRhiTestDraw;

struct RhiTestVertex
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	nointerpolation float4 color : COLOR0;
};

RhiTestVertex Place(RhiTestDrawConstants draw, uint vertexId)
{
	const float2 corners[3] = {float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0)};
	RhiTestVertex output;
	const float2 corner = corners[vertexId];
	output.position = float4((corner * draw.scale) + draw.offset, draw.depth, 1.0);
	output.uv = (corner * 0.5) + 0.5;
	output.color = draw.color;
	return output;
}

[shader("vertex")]
RhiTestVertex VertexRhiTestDraw(uint vertexId: SV_VertexID)
{
	return Place(gRhiTestDraw, vertexId);
}

[shader("vertex")]
RhiTestVertex VertexRhiTestDrawIndirect(uint vertexId: SV_VertexID, uint record: SV_StartInstanceLocation)
{
	return Place(gSetRhiTestDraw.gDraws[record], vertexId);
}

[shader("fragment")]
float4 FragmentRhiTestDraw(RhiTestVertex input) : SV_Target
{
	return float4(input.uv, 1.0 - input.uv.x, 1.0) * input.color;
}
