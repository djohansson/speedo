// rhitest's kernel (tools/rhitest.cpp): each thread writes its index times 3 plus the push constant's offset. bound as
// the client's layouts are: the push constants in set 0, set 1's resources a parameter block (see bindings.slang)

struct RhiTestConstants
{
	uint offset;
	uint count;
};

[[vk::binding(0, 0)]]
[[vk::push_constant]]
ConstantBuffer<RhiTestConstants> gRhiTest;

struct RhiTestSet
{
	RWStructuredBuffer<uint> gOutput;
};
[[vk::binding(0, 1)]]
ParameterBlock<RhiTestSet> gSetRhiTest;

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeRhiTest(uint3 dispatchId: SV_DispatchThreadID)
{
	if (dispatchId.x < gRhiTest.count)
		gSetRhiTest.gOutput[dispatchId.x] = dispatchId.x * 3u + gRhiTest.offset;
}
