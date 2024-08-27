#include "imgui_extra.h"

#include <imgui_internal.h> // ImPool<>, ImHashData

namespace imgui_extra
{

ImDrawDataSnapshot::ImDrawDataSnapshot()
	: Cache(IM_NEW(ImPool<ImDrawDataSnapshotEntry>))
{}

ImDrawDataSnapshot::~ImDrawDataSnapshot()
{
	Clear();

	IM_FREE(Cache);
}

void ImDrawDataSnapshot::Clear()
{
	for (int n = 0; n < Cache->GetMapSize(); n++)
		if (ImDrawDataSnapshotEntry* entry = Cache->TryGetMapData(n))
			IM_DELETE(entry->OurCopy);

	Cache->Clear();
}

void ImDrawDataSnapshot::SnapUsingSwap(ImDrawData* src, ImDrawData* dst, double current_time)
{
	IM_ASSERT(src != dst && src->Valid);

	// Copy all fields except CmdLists[]
	ImVector<ImDrawList*> backup_draw_list;
	backup_draw_list.swap(src->CmdLists);
	IM_ASSERT(src->CmdLists.Data == NULL);
	*dst = *src;
	backup_draw_list.swap(src->CmdLists);

	// Swap and mark as used
	for (ImDrawList* src_list : src->CmdLists)
	{
		ImDrawDataSnapshotEntry* entry = GetOrAddEntry(src_list);
		if (entry->OurCopy == NULL)
		{
			entry->SrcCopy = src_list;
			entry->OurCopy = IM_NEW(ImDrawList)(src_list->_Data);
		}
		IM_ASSERT(entry->SrcCopy == src_list);
		entry->SrcCopy->CmdBuffer.swap(entry->OurCopy->CmdBuffer); // Cheap swap
		entry->SrcCopy->IdxBuffer.swap(entry->OurCopy->IdxBuffer);
		entry->SrcCopy->VtxBuffer.swap(entry->OurCopy->VtxBuffer);
		entry->SrcCopy->CmdBuffer.reserve(entry->OurCopy->CmdBuffer.Capacity); // Preserve bigger size to avoid reallocs for two consecutive frames
		entry->SrcCopy->IdxBuffer.reserve(entry->OurCopy->IdxBuffer.Capacity);
		entry->SrcCopy->VtxBuffer.reserve(entry->OurCopy->VtxBuffer.Capacity);
		entry->LastUsedTime = current_time;
		dst->CmdLists.push_back(entry->OurCopy);
	}

	// Cleanup unused data
	const double gc_threshold = current_time - MemoryCompactTimer;
	for (int n = 0; n < Cache->GetMapSize(); n++)
		if (ImDrawDataSnapshotEntry* entry = Cache->TryGetMapData(n))
		{
			if (entry->LastUsedTime > gc_threshold)
				continue;

			IM_DELETE(entry->OurCopy);
			
			Cache->Remove(GetDrawListID(entry->SrcCopy), entry);
		}
}

ImGuiID ImDrawDataSnapshot::GetDrawListID(ImDrawList* src_list)
{
	return ImHashData(&src_list, sizeof(src_list));
}

ImDrawDataSnapshotEntry* ImDrawDataSnapshot::GetOrAddEntry(ImDrawList* src_list)
{
	return Cache->GetOrAddByKey(GetDrawListID(src_list));
}

} // namespace imgui_extra
