// WARP 1.0.18 to 1.0.22 (and 1.65535.22-preview) remove the device when this shader is dispatched; 1.0.13 to 1.0.17
// and Windows 11's d3d10warp.dll (10.0.26100) run it. dxc -T cs_6_8 -E main repro.hlsl -Fo repro.dxil
// WARP's error log (its RecordError ring): ParseSubroutineDXIL line 3708 <- Implement_DXIL_BufferLoad line 12703
// (Implement_DXIL_TextureLoad line 12461 with a texture's Load in its place), E_FAIL, then the device removed from
// RenderSubmission::DispatchMain (CompileComputePipeline at the dispatch)
cbuffer Push : register(b0) { uint count; };
StructuredBuffer<float4> gIn : register(t0);
RWStructuredBuffer<float4> gOut : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	float4 v = 0;
	uint it = count;
	for (;;)
	{
		if (it == 0)
			return; // one exit: to the end
		it--;
		v = gIn.Load(it);
		if (v.x > 0.5f)
			break; // the other: to the store, which reads the load's result after the loop
	}
	gOut[0] = v;
	// passes instead: no load in the loop (a value from the counter); the first exit a break (both exits to the
	// store); only v.x read after the loop (its extractvalue then stays in the loop)
}
