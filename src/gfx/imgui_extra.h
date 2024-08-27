#pragma once

#include <imgui.h>

template<typename T>
struct ImPool;

namespace imgui_extra
{

struct ImDrawDataSnapshotEntry
{
	ImDrawList* SrcCopy = nullptr;
	ImDrawList* OurCopy = nullptr;
	double LastUsedTime = 0.0;
};

struct ImDrawDataSnapshot
{
	ImPool<ImDrawDataSnapshotEntry>* Cache;
	float MemoryCompactTimer = 20.0f;

	ImDrawDataSnapshot();
	~ImDrawDataSnapshot();

	void Clear();
	void SnapUsingSwap(ImDrawData* src, ImDrawData* dst, double current_time); 

	// Internals
	ImGuiID GetDrawListID(ImDrawList* src_list);
	ImDrawDataSnapshotEntry* GetOrAddEntry(ImDrawList* src_list);
};

} // namespace imgui_extra
