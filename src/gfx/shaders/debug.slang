// the debug views' helpers (PushConstants::debugView, DEBUG_VIEW_*): which one is shown, how ComputeMain maps it, and
// their colors. a part of shaders.slang, which includes it after capi.h and sky.h

// the frame's debug view (DEBUG_VIEW_NONE: the light)
uint DebugView()
{
	return gPushConstants.debugView;
}

// whether a debug view shows values rather than light: ComputeMain shows them as they are (clamped, then the srgb curve:
// a base color looks as it does in a texture), without exposure or tonemapping. by a mask of the views (32 to a word),
// its bit taken with a shift within the word, not by a switch: DXC makes a switch of constants a mask of its own, shifted
// by the view less its first case without the range check before it (lshr i32 mask, debugView - 1: by 0xffffffff for
// DEBUG_VIEW_NONE, every frame's; a shift past the width has no defined value in LLVM, and DXIL's specification gives it
// none), and the range check anded with what comes of it. NVIDIA's driver (RTX 4070 Ti SUPER, 2026-11) then ran neither
// side of the branches on the result in some frames: ComputeMain and ComputeSharpen wrote nothing, and the window showed
// the image's last frame instead (the flicker on D3D12 while flying, the user's report of 2026-11-15; see
// docs/notes/rendering.md)
#define DEBUG_VIEW_DATA_WORD(view, word) ((((view) >> 5u) == (word)) ? (1u << ((view) & 31u)) : 0u)
uint DebugViewDataWord(uint word)
{
	uint data = 0u;
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_BASE_COLOR, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_NORMAL, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_GEOMETRY_NORMAL, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_ROUGHNESS, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_METALLIC, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_OCCLUSION, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_MATERIAL, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_TEXCOORD, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_SHADOWS, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_LIGHT_COUNT, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_GI_VISIBILITY, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_GI_VOXEL_ALBEDO, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_GI_CLIP, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_GI_VOXEL_WORLD_ALBEDO, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_GI_VOXEL_WORLD_NORMAL, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_GI_PROBE_STATE, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_TRACED_SHADOW, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_TRACED_SHADOW_RAW, word);
	data |= DEBUG_VIEW_DATA_WORD(DEBUG_VIEW_TRACED_SHADOW_REFERENCE, word);
	return data;
}

bool DebugViewIsData(uint debugView)
{
	uint word = debugView >> 5u;
	uint data = word == 0u ? DebugViewDataWord(0u) : word == 1u ? DebugViewDataWord(1u) : 0u;
	return (data & (1u << (debugView & 31u))) != 0u;
}

// the indirect lighting's debug view (GiData::debug's numbering: 1 and up), or 0
uint DebugViewGi(uint debugView)
{
	return debugView >= DEBUG_VIEW_GI_FIRST && debugView < DEBUG_VIEW_GI_END ? debugView - DEBUG_VIEW_GI_FIRST + 1u : 0u;
}

// a heat map of t in [0, 1]: black, blue, cyan, green, yellow, red (stored linear, so its steps look even once the srgb
// curve is applied)
float3 DebugHeat(float t)
{
	t = saturate(t);
	float3 color;
	if (t < 0.2)
		color = lerp(float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 1.0), t / 0.2);
	else if (t < 0.4)
		color = lerp(float3(0.0, 0.0, 1.0), float3(0.0, 1.0, 1.0), (t - 0.2) / 0.2);
	else if (t < 0.6)
		color = lerp(float3(0.0, 1.0, 1.0), float3(0.0, 1.0, 0.0), (t - 0.4) / 0.2);
	else if (t < 0.8)
		color = lerp(float3(0.0, 1.0, 0.0), float3(1.0, 1.0, 0.0), (t - 0.6) / 0.2);
	else
		color = lerp(float3(1.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), (t - 0.8) / 0.2);
	return color * color; // linear
}

// a color of its own for an index (a material's), bright enough to tell apart
float3 DebugIndexColor(uint index)
{
	uint hash = index * 747796405u + 2891336453u;
	hash = ((hash >> ((hash >> 28u) + 4u)) ^ hash) * 277803737u;
	hash = (hash >> 22u) ^ hash;
	float3 color = float3(hash & 0xFFu, (hash >> 8u) & 0xFFu, (hash >> 16u) & 0xFFu) / 255.0;
	color = 0.2 + 0.8 * color;
	return color * color; // linear
}

// the indirect lighting's clips, finest first (DEBUG_VIEW_GI_CLIP)
float3 DebugClipColor(uint level)
{
	switch (level)
	{
	case 0u: return float3(1.0, 0.1, 0.1);
	case 1u: return float3(0.1, 1.0, 0.1);
	case 2u: return float3(0.1, 0.2, 1.0);
	default: return float3(1.0, 1.0, 0.1);
	}
}
