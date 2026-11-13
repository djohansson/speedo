// rhitest's draw (tools/rhitest.cpp): a triangle over the target's upper left half (as Vulkan's clip space has it, y
// down), shaded by where it is (its interpolated coordinates times the draw's color), at the draw's depth, offset and
// scaled by the draw: asymmetric, so that a backend drawing it mirrored or wound the other way differs from Vulkan's

struct RhiTestDrawConstants
{
	float4 color;
	float2 offset;
	float2 scale; // per axis: a negative one mirrors the triangle, winding it the other way
	float depth;
};

[[vk::binding(0, 0)]]
[[vk::push_constant]]
ConstantBuffer<RhiTestDrawConstants> gRhiTestDraw;

struct RhiTestVertex
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

[shader("vertex")]
RhiTestVertex VertexRhiTestDraw(uint vertexId: SV_VertexID)
{
	const float2 corners[3] = {float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0)};
	RhiTestVertex output;
	const float2 corner = corners[vertexId];
	output.position = float4((corner * gRhiTestDraw.scale) + gRhiTestDraw.offset, gRhiTestDraw.depth, 1.0);
	output.uv = (corner * 0.5) + 0.5;
	return output;
}

[shader("fragment")]
float4 FragmentRhiTestDraw(RhiTestVertex input) : SV_Target
{
	return float4(input.uv, 1.0 - input.uv.x, 1.0) * gRhiTestDraw.color;
}
