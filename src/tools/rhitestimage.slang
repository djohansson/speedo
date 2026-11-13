// rhitest's image kernel (tools/rhitest.cpp): each thread reads its texel of an uploaded image twice, loaded and
// sampled at the texel's center (nearest), and writes the first, its channels reversed and doubled, into a storage
// image, the second into a buffer. bound as the client's layouts are: the push constants in set 0, set 1's resources a
// parameter block (see bindings.slang)

struct RhiTestImageConstants
{
	uint width;
	uint height;
};

[[vk::binding(0, 0)]]
[[vk::push_constant]]
ConstantBuffer<RhiTestImageConstants> gRhiTestImage;

struct RhiTestImageSet
{
	Texture2D<float4> gSource;
	SamplerState gSampler;
	[[vk::image_format("rgba32f")]] RWTexture2D<float4> gTarget;
	RWStructuredBuffer<float4> gSampled;
};
[[vk::binding(0, 1)]]
ParameterBlock<RhiTestImageSet> gSetRhiTestImage;

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeRhiTestImage(uint3 dispatchId: SV_DispatchThreadID)
{
	const uint2 size = uint2(gRhiTestImage.width, gRhiTestImage.height);
	if (any(dispatchId.xy >= size))
		return;
	float4 texel = gSetRhiTestImage.gSource.Load(int3(dispatchId.xy, 0));
	gSetRhiTestImage.gTarget[dispatchId.xy] = texel.wzyx * 2.0;
	float2 uv = (float2(dispatchId.xy) + 0.5) / float2(size);
	gSetRhiTestImage.gSampled[dispatchId.y * size.x + dispatchId.x] = gSetRhiTestImage.gSource.SampleLevel(gSetRhiTestImage.gSampler, uv, 0.0);
}
