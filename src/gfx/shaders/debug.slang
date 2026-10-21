// the debug views' helpers (PushConstants::debugView, DEBUG_VIEW_*): which one is shown, how ComputeMain maps it, and
// their colors. a part of shaders.slang, which includes it after capi.h and sky.h

// the frame's debug view (DEBUG_VIEW_NONE: the light)
uint DebugView()
{
	return gPushConstants.debugView;
}

// whether a debug view shows values rather than light: ComputeMain shows them as they are (clamped, then the srgb curve:
// a base color looks as it does in a texture), without exposure or tonemapping
bool DebugViewIsData(uint debugView)
{
	switch (debugView)
	{
	case DEBUG_VIEW_BASE_COLOR:
	case DEBUG_VIEW_NORMAL:
	case DEBUG_VIEW_GEOMETRY_NORMAL:
	case DEBUG_VIEW_ROUGHNESS:
	case DEBUG_VIEW_METALLIC:
	case DEBUG_VIEW_OCCLUSION:
	case DEBUG_VIEW_MATERIAL:
	case DEBUG_VIEW_TEXCOORD:
	case DEBUG_VIEW_SHADOWS:
	case DEBUG_VIEW_LIGHT_COUNT:
	case DEBUG_VIEW_GI_VISIBILITY:
	case DEBUG_VIEW_GI_VOXEL_ALBEDO:
	case DEBUG_VIEW_GI_CLIP:
	case DEBUG_VIEW_GI_VOXEL_WORLD_ALBEDO:
	case DEBUG_VIEW_GI_VOXEL_WORLD_NORMAL:
	case DEBUG_VIEW_GI_PROBE_STATE:
		return true;
	default:
		return false;
	}
}

// the indirect lighting's debug view (GiData::debug's numbering: 1 and up), or 0
uint DebugViewGi(uint debugView)
{
	return debugView >= DEBUG_VIEW_GI_FIRST && debugView < DEBUG_VIEW_COUNT ? debugView - DEBUG_VIEW_GI_FIRST + 1u : 0u;
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
float3 DebugClipColor(uint clip)
{
	switch (clip)
	{
	case 0u: return float3(1.0, 0.1, 0.1);
	case 1u: return float3(0.1, 1.0, 0.1);
	case 2u: return float3(0.1, 0.2, 1.0);
	default: return float3(1.0, 1.0, 0.1);
	}
}
