#include "imgui_extra.h"

#include <imgui_internal.h> // ImPool<>, ImHashData

namespace imgui_extra
{

ImDrawDataSnapshot::ImDrawDataSnapshot()
	: myCache(IM_NEW(ImPool<ImDrawDataSnapshotEntry>))
{}

ImDrawDataSnapshot::~ImDrawDataSnapshot()
{
	Clear();

	IM_FREE(myCache);
}

void ImDrawDataSnapshot::Clear()
{
	for (int mapDataIt = 0; mapDataIt < myCache->GetMapSize(); mapDataIt++)
		if (ImDrawDataSnapshotEntry* entry = myCache->TryGetMapData(mapDataIt))
			IM_DELETE(entry->OurCopy);

	myCache->Clear();
}

void ImDrawDataSnapshot::SnapUsingSwap(ImDrawData* src, ImDrawData* dst, double currentTime)
{
	IM_ASSERT(src != dst && src->Valid);

	// Copy all fields except CmdLists[]
	ImVector<ImDrawList*> backupDrawList;
	backupDrawList.swap(src->CmdLists);
	IM_ASSERT(src->CmdLists.Data == nullptr);
	*dst = *src;
	backupDrawList.swap(src->CmdLists);

	// Swap and mark as used
	for (ImDrawList* srcList : src->CmdLists)
	{
		ImDrawDataSnapshotEntry* entry = InternalGetOrAddEntry(srcList);
		if (entry->OurCopy == nullptr)
		{
			entry->SrcCopy = srcList;
			entry->OurCopy = IM_NEW(ImDrawList)(srcList->_Data);
		}
		IM_ASSERT(entry->SrcCopy == srcList);
		entry->SrcCopy->CmdBuffer.swap(entry->OurCopy->CmdBuffer); // Cheap swap
		entry->SrcCopy->IdxBuffer.swap(entry->OurCopy->IdxBuffer);
		entry->SrcCopy->VtxBuffer.swap(entry->OurCopy->VtxBuffer);
		entry->SrcCopy->CmdBuffer.reserve(entry->OurCopy->CmdBuffer.Capacity); // Preserve bigger size to avoid reallocs for two consecutive frames
		entry->SrcCopy->IdxBuffer.reserve(entry->OurCopy->IdxBuffer.Capacity);
		entry->SrcCopy->VtxBuffer.reserve(entry->OurCopy->VtxBuffer.Capacity);
		entry->LastUsedTime = currentTime;
		dst->CmdLists.push_back(entry->OurCopy);
	}

	// Cleanup unused data
	const double gcThreshold = currentTime - myMemoryCompactTimer;
	for (int mapDataIt = 0; mapDataIt < myCache->GetMapSize(); mapDataIt++)
		if (ImDrawDataSnapshotEntry* entry = myCache->TryGetMapData(mapDataIt))
		{
			if (entry->LastUsedTime > gcThreshold)
				continue;

			IM_DELETE(entry->OurCopy);
			
			myCache->Remove(InternalGetDrawListID(entry->SrcCopy), entry);
		}
}

ImGuiID ImDrawDataSnapshot::InternalGetDrawListID(ImDrawList* srcList)
{
	return ImHashData(static_cast<const void*>(&srcList), sizeof(srcList)); //NOLINT(bugprone-sizeof-expression)
}

ImDrawDataSnapshotEntry* ImDrawDataSnapshot::InternalGetOrAddEntry(ImDrawList* srcList)
{
	return myCache->GetOrAddByKey(InternalGetDrawListID(srcList));
}

} // namespace imgui_extra
