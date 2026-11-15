// a vertex stage's position in the target's clip space: rhi's is Vulkan's, y down; D3D's is y up, and D3D12's viewport
// can't flip it (a negative height, as Vulkan's and Metal's backends flip theirs), so for a target whose clip space is y up
// (SHADER_TARGET_CLIP_Y_UP, the shader compiler's for DXIL) the vertex stage flips it: the pixels are where Vulkan's are,
// and so the windings and front faces (in framebuffer coordinates on both)
#pragma once

float4 ClipSpace(float4 position)
{
#if defined(SHADER_TARGET_CLIP_Y_UP)
	return float4(position.x, -position.y, position.z, position.w);
#else
	return position;
#endif
}
