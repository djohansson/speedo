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

class ImDrawDataSnapshot
{
public:
	ImDrawDataSnapshot();
	~ImDrawDataSnapshot();

	void Clear();
	void SnapUsingSwap(ImDrawData* src, ImDrawData* dst, double currentTime); 

private:
	static ImGuiID InternalGetDrawListID(ImDrawList* srcList);
	ImDrawDataSnapshotEntry* InternalGetOrAddEntry(ImDrawList* srcList);

	ImPool<ImDrawDataSnapshotEntry>* myCache;
	float myMemoryCompactTimer = 20.0F; //NOLINT(readability-magic-numbers)
};

} // namespace imgui_extra
