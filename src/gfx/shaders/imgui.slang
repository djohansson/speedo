// the ui's draws (the ImGui layout, see gfx::ImGuiRenderer): imgui's triangles, their vertices read from the frame
// slot's buffer by index (rhi's pipelines have no vertex input), each draw's texture from gImGuiTextures by its slot

#include "capi.h"

[[vk::binding(0, 0)]]
[[vk::push_constant]]
ConstantBuffer<ImGuiConstants> gImGui;

// per frame slot, imgui's vertices (ImDrawVert), SHADER_TYPES_IMGUI_VERTEX_WORD_COUNT words each: the position (x, y), the
// texture coordinates (u, v) and the color (8 bits a channel), as capi.h lays them out
[[vk::binding(0, 1)]]
StructuredBuffer<uint> gImGuiVertices[SHADER_TYPES_FRAME_COUNT];
// imgui's textures (its font atlas) and those the ui shows (ImGuiRenderer::AddTexture), by slot
[[vk::binding(1, 1)]]
Texture2D gImGuiTextures[SHADER_TYPES_IMGUI_TEXTURE_COUNT];
// linear, clamped to the edge (as imgui's own backends sample)
[[vk::binding(2, 1)]]
SamplerState gImGuiSampler;

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
	uint first = (vertexId + gImGui.vertexBase) * SHADER_TYPES_IMGUI_VERTEX_WORD_COUNT;
	StructuredBuffer<uint> vertices = gImGuiVertices[gImGui.frame];
	uint position = first + SHADER_TYPES_IMGUI_VERTEX_POSITION_WORD;
	uint uv = first + SHADER_TYPES_IMGUI_VERTEX_UV_WORD;
	uint color = vertices[first + SHADER_TYPES_IMGUI_VERTEX_COLOR_WORD];

	ImGuiVertexOutput output;
	output.position = float4((asfloat(uint2(vertices[position], vertices[position + 1u])) * gImGui.scale) + gImGui.translate, 0.0, 1.0);
	output.color = float4(
		(color >> SHADER_TYPES_IMGUI_COLOR_R_SHIFT) & 0xffu,
		(color >> SHADER_TYPES_IMGUI_COLOR_G_SHIFT) & 0xffu,
		(color >> SHADER_TYPES_IMGUI_COLOR_B_SHIFT) & 0xffu,
		(color >> SHADER_TYPES_IMGUI_COLOR_A_SHIFT) & 0xffu) / 255.0;
	output.uv = asfloat(uint2(vertices[uv], vertices[uv + 1u]));
	return output;
}

[shader("fragment")]
float4 FragmentImGui(ImGuiVertexOutput input) : SV_Target
{
	return input.color * gImGuiTextures[gImGui.texture].Sample(gImGuiSampler, input.uv);
}
