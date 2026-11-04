// the ui's draws (the ImGui layout, see gfx::ImGuiRenderer): imgui's triangles, their vertices read from the frame
// slot's buffer by index (rhi's pipelines have no vertex input), each draw's texture from gImGuiTextures by its slot

#include "capi.h"

[[vk::binding(0, 0)]]
[[vk::push_constant]]
ConstantBuffer<ImGuiConstants> gImGui;

// per frame slot, imgui's vertices (ImDrawVert), 5 words each: the position (x, y), the texture coordinates (u, v) and
// the color (rgba, 8 bits each, r in the low bits)
[[vk::binding(0, 1)]]
StructuredBuffer<uint> gImGuiVertices[SHADER_TYPES_FRAME_COUNT];
// imgui's textures (its font atlas) and those the ui shows (ImGuiRenderer::AddTexture), by slot
[[vk::binding(1, 1)]]
Texture2D gImGuiTextures[SHADER_TYPES_IMGUI_TEXTURE_COUNT];
// linear, clamped to the edge (as imgui's own backends sample)
[[vk::binding(2, 1)]]
SamplerState gImGuiSampler;

static const uint kImGuiVertexWordCount = 5u;

struct ImGuiVertexOutput
{
	float4 position : SV_Position;
	float4 color : COLOR;
	float2 uv : TEXCOORD0;
};

[shader("vertex")]
ImGuiVertexOutput VertexImGui(uint vertexId : SV_VertexID)
{
	// the index (the draws' vertex offset is 0), plus the draw's first vertex
	uint first = (vertexId + gImGui.vertexBase) * kImGuiVertexWordCount;
	StructuredBuffer<uint> vertices = gImGuiVertices[gImGui.frame];
	float2 position = asfloat(uint2(vertices[first], vertices[first + 1u]));
	uint color = vertices[first + 4u];

	ImGuiVertexOutput output;
	output.position = float4((position * gImGui.scale) + gImGui.translate, 0.0, 1.0);
	output.color = float4(color & 0xffu, (color >> 8u) & 0xffu, (color >> 16u) & 0xffu, color >> 24u) / 255.0;
	output.uv = asfloat(uint2(vertices[first + 2u], vertices[first + 3u]));
	return output;
}

[shader("fragment")]
float4 FragmentImGui(ImGuiVertexOutput input) : SV_Target
{
	return input.color * gImGuiTextures[gImGui.texture].Sample(gImGuiSampler, input.uv);
}
