#include "rhi/deviceobject.h"
#include <rhi/command.h>
#include <rhi/device.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

#include <format>
#include <utility>

#include <vulkan/vulkan_core.h>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(CommandBufferArray<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(CommandBufferArray<kVk>);
IMPLEMENT_OBJECT_GETINSTANCE(CommandPool<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(CommandPool<kVk>);

namespace commandbufferarray
{

static auto
CreateArray(const Device<kVk>& device, const CommandBufferArrayCreateDesc<kVk>& desc)
{
	ZoneScopedN("commandbufferarray::createArray");

	std::array<CommandBufferHandle<kVk>, CommandBufferArray<kVk>::Capacity()> outArray;

	{
		ZoneScopedN("commandbufferarray::createArray::vkAllocateCommandBuffers");

		VkCommandBufferAllocateInfo cmdInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cmdInfo.commandPool = desc.pool;
		cmdInfo.level = desc.level == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
		cmdInfo.commandBufferCount = CommandBufferArray<kVk>::Capacity();
		VK_CHECK(vkAllocateCommandBuffers(device, &cmdInfo, outArray.data()));
	}

	for (size_t cmdIt = 0; cmdIt < outArray.size(); cmdIt++)
		Track(device, VK_OBJECT_TYPE_COMMAND_BUFFER, outArray[cmdIt], std::format("{} {}", GetDebugName(desc), cmdIt));

	return outArray;
}

} // namespace commandbufferarray

template <>
void CommandBufferArray<kVk>::Swap(CommandBufferArray& rhs) noexcept
{
	DeviceObject<CommandBufferArray<kVk>>::Swap(rhs);
	std::swap(myArray, rhs.myArray);
	std::swap(myBits, rhs.myBits);
}

template <>
CommandBufferArray<kVk>::CommandBufferArray(
	CreateDescType&& desc,
	std::array<CommandBufferHandle<kVk>, kCommandBufferCount>&& array)
	: DeviceObject<CommandBufferArray<kVk>>(std::forward<CreateDescType>(desc))
	, myArray(std::forward<std::array<CommandBufferHandle<kVk>, kCommandBufferCount>>(array))
{}

template <>
CommandBufferArray<kVk>::CommandBufferArray(CreateDescType&& desc)
	: CommandBufferArray(
		std::forward<CreateDescType>(desc),
		commandbufferarray::CreateArray(GetDevice(desc.device), desc))
{}

template <>
CommandBufferArray<kVk>::CommandBufferArray(CommandBufferArray&& other) noexcept
{
	Swap(other);
}

template <>
CommandBufferArray<kVk>::~CommandBufferArray()
{
	ZoneScopedN("~CommandBufferArray()");

	if (IsValid())
	{
		ZoneScopedN("~CommandBufferArray()::vkFreeCommandBuffers");

		for (auto* cmd : myArray)
			Untrack(VK_OBJECT_TYPE_COMMAND_BUFFER, cmd);
		vkFreeCommandBuffers(
			GetDevice(), GetDesc().pool, kCommandBufferCount, myArray.data());
	}
}

template <>
CommandBufferArray<kVk>& CommandBufferArray<kVk>::operator=(CommandBufferArray&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
void CommandBufferArray<kVk>::Reset()
{
	ZoneScopedN("CommandBufferArray::reset");

	ENSURE(!RecordingFlags());
	ENSURE(Head() < kCommandBufferCount);

	if (GetDesc().useResetCommandBuffers)
	{
		for (uint32_t i = 0UL; i < Head(); i++)
		{
			ZoneScopedN("CommandBufferArray::reset::vkResetCommandBuffer");

			VK_CHECK(
				vkResetCommandBuffer(myArray[i], 
					GetDesc().useReleaseResourcesOnReset ? VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT : 0));
		}
	}

	myBits = {.head = 0, .recordingFlags = 0};
}

template <>
uint8_t CommandBufferArray<kVk>::Begin(const CommandBufferBeginInfo<kVk>& beginInfo)
{
	ZoneScopedN("CommandBufferArray::begin");

	ENSURE(!Recording(myBits.head));
	ENSURE(!Full());

	VK_CHECK(vkBeginCommandBuffer(myArray[myBits.head], &beginInfo));

	myBits.recordingFlags |= (1 << myBits.head);

	return (myBits.head++);
}

template <>
void CommandBufferArray<kVk>::End(uint8_t index)
{
	ZoneScopedN("CommandBufferArray::end");

	ENSURE(Recording(index));

	myBits.recordingFlags &= ~(1 << index);

	VK_CHECK(vkEndCommandBuffer(myArray[index]));
}

template <>
CommandPool<kVk>::CommandPool(
	CreateDescType&& desc,
	CommandPoolHandle<kVk>&& pool)
	: DeviceObject<CommandPool<kVk>>(std::forward<typename CommandPool<kVk>::CreateDescType>(desc))
	, myPool(std::forward<CommandPoolHandle<kVk>>(pool))
	, myPendingCommands(GetDesc().levelCount)
	, mySubmittedCommands(GetDesc().levelCount)
	, myFreeCommands(GetDesc().levelCount)
	, myRecordingCommands(GetDesc().levelCount)
{
	ASSERT(GetDesc().levelCount > 0);
	ASSERT(myPool != VK_NULL_HANDLE);
}

template <>
CommandPool<kVk>::CommandPool(
	CreateDescType&& desc)
	: CommandPool(
		std::forward<CreateDescType>(desc),
		[this, &desc]
		{
			VkCommandPoolCreateInfo cmdPoolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			cmdPoolInfo.flags = desc.flags;
			cmdPoolInfo.queueFamilyIndex = desc.queueFamilyIndex;

			VkCommandPool outPool;
			VK_CHECK(vkCreateCommandPool(
				desc.device,
				&cmdPoolInfo,
				&GetInstance().GetHostAllocationCallbacks(),
				&outPool));

			Track(desc.device, VK_OBJECT_TYPE_COMMAND_POOL, outPool, GetDebugName(desc));

			return outPool;
		}())
{}

template <>
void CommandPool<kVk>::Swap(CommandPool& other) noexcept
{
	DeviceObject<CommandPool<kVk>>::Swap(other);
	std::swap(myPool, other.myPool);
	std::swap(myPendingCommands, other.myPendingCommands);
	std::swap(mySubmittedCommands, other.mySubmittedCommands);
	std::swap(myFreeCommands, other.myFreeCommands);
	std::swap(myRecordingCommands, other.myRecordingCommands);
}

template <>
CommandPool<kVk>::CommandPool(CommandPool&& other) noexcept
{
	Swap(other);
}

template <>
CommandPool<kVk>::~CommandPool()
{
	if (!IsValid())
		return;

	// members are only destroyed after this body has run, so free the command buffers before their pool goes away
	myRecordingCommands.clear();
	myPendingCommands.clear();
	mySubmittedCommands.clear();
	myFreeCommands.clear();

	Untrack(VK_OBJECT_TYPE_COMMAND_POOL, myPool);
	vkDestroyCommandPool(
		GetDevice(),
		myPool,
		&GetInstance().GetHostAllocationCallbacks());
}

template <>
CommandPool<kVk>& CommandPool<kVk>::operator=(CommandPool&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
void CommandPool<kVk>::Reset()
{
	ZoneScopedN("CommandPool::reset");

	constexpr bool kUseReleaseResources = true;

	if ((GetDesc().flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) != 0U)
	{
		ZoneScopedN("CommandPool::reset::vkResetCommandPool");

		VK_CHECK(vkResetCommandPool(
			GetDevice(),
			myPool,
			kUseReleaseResources ? VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT : 0));
	}

	for (uint32_t levelIt = 0UL; levelIt < mySubmittedCommands.size(); levelIt++)
	{
		auto& submittedCommandList = mySubmittedCommands[levelIt];

		for (auto& commands : submittedCommandList)
			std::get<0>(commands).Reset();

		auto& freeCommandList = myFreeCommands[levelIt];

		freeCommandList.splice(freeCommandList.end(), std::move(submittedCommandList));
	}
}

template <>
void CommandPool<kVk>::InternalEnqueueOnePending(uint8_t level)
{
	ZoneScopedN("CommandPool::InternalEnqueueOnePending");

	if (!myFreeCommands[level].empty())
	{
		myPendingCommands[level].splice(
			myPendingCommands[level].end(), myFreeCommands[level], myFreeCommands[level].begin());
	}
	else
	{
		// char stringBuffer[32];

		// std::format_to_n(
		// 	stringBuffer,
		// 	std::size(stringBuffer),
		// 	"{0}{1}",
		// 	level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ? "Primary" : "Secondary",
		// 	"CommandBufferArray");

		myPendingCommands[level].emplace_back(std::make_tuple(
			CommandBufferArray<kVk>(
				CommandBufferArrayCreateDesc<kVk>{
					SuperType::CreateDeviceObjectCreateDesc(std::format("{}CommandBufferArray", level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ? "Primary" : "Secondary")),
					myPool,
					level,
					static_cast<uint8_t>((GetDesc().flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == 0U),
					1}),
			0));
	}
}

template <>
CommandBufferAccessScope<kVk>
CommandPool<kVk>::InternalBeginScope(const CommandBufferAccessScopeDesc<kVk>& beginInfo)
{
	if (myPendingCommands[beginInfo.level].empty() ||
		std::get<0>(myPendingCommands[beginInfo.level].back()).Full())
		InternalEnqueueOnePending(beginInfo.level);

	return myRecordingCommands[beginInfo.level].emplace(CommandBufferAccessScope(
		beginInfo,
		&std::get<0>(myPendingCommands[beginInfo.level].back())));
}

template <>
void CommandPool<kVk>::InternalEnqueueSubmitted(
	CommandBufferListType<kVk>&& cbList, uint8_t level, uint64_t timelineValue)
{
	ZoneScopedN("CommandPool::InternalEnqueueSubmitted");

	for (auto& [cmdArray, cmdTimelineValue] : cbList)
		cmdTimelineValue = timelineValue;

	mySubmittedCommands[level].splice(
		mySubmittedCommands[level].end(), std::forward<CommandBufferListType<kVk>>(cbList));
}

template <>
CommandBufferAccessScopeDesc<kVk>::CommandBufferAccessScopeDesc(bool scopedBeginEnd) noexcept
	: CommandBufferBeginInfo<kVk>{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pNext = nullptr,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		.pInheritanceInfo = &inheritance}
	, inheritance{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}
	, level(0)
	, scopedBeginEnd(scopedBeginEnd)
{}

template <>
CommandBufferAccessScopeDesc<kVk>::CommandBufferAccessScopeDesc(
	const CommandBufferAccessScopeDesc& other) noexcept
	: CommandBufferBeginInfo<kVk>(other)
	, inheritance(other.inheritance)
	, level(other.level)
	, scopedBeginEnd(other.scopedBeginEnd)
{
	pInheritanceInfo = &inheritance;
}

template <>
CommandBufferAccessScopeDesc<kVk>&
CommandBufferAccessScopeDesc<kVk>::operator=(const CommandBufferAccessScopeDesc& other) noexcept
{
	*static_cast<CommandBufferBeginInfo<kVk>*>(this) = other;
	inheritance = other.inheritance;
	level = other.level;
	scopedBeginEnd = other.scopedBeginEnd;
	pInheritanceInfo = &inheritance;
	return *this;
}

template <>
bool CommandBufferAccessScopeDesc<kVk>::operator==(const CommandBufferAccessScopeDesc& other) const noexcept
{
	bool result = true;

	if (this != &other)
	{
		result = other.flags == flags && other.level == level && scopedBeginEnd == other.scopedBeginEnd;
		if (result && level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
		{
			ENSURE(pInheritanceInfo != nullptr);
			result &=
				other.pInheritanceInfo->renderPass == pInheritanceInfo->renderPass &&
				other.pInheritanceInfo->subpass == pInheritanceInfo->subpass &&
				other.pInheritanceInfo->framebuffer == pInheritanceInfo->framebuffer &&
				other.pInheritanceInfo->occlusionQueryEnable ==
					pInheritanceInfo->occlusionQueryEnable &&
				other.pInheritanceInfo->queryFlags == pInheritanceInfo->queryFlags &&
				other.pInheritanceInfo->pipelineStatistics == pInheritanceInfo->pipelineStatistics;
		}
	}

	return result;
}

template <>
CommandBufferAccessScope<kVk>
CommandPool<kVk>::SecondaryCommands(uint8_t level, const RenderTargetBeginInfo<kVk>& renderTarget)
{
	ENSURE(level > 0);

	// only needs to live while the scope begins (in Commands)
	VkCommandBufferInheritanceInfo inheritInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
	if (const auto* dynamicRenderingInfo = std::get_if<DynamicRenderingInfo<kVk>>(&renderTarget))
	{
		inheritInfo.pNext = &dynamicRenderingInfo->inheritanceInfo;
	}
	else if (const auto* renderPassBeginInfo = std::get_if<VkRenderPassBeginInfo>(&renderTarget))
	{
		inheritInfo.renderPass = renderPassBeginInfo->renderPass;
		inheritInfo.framebuffer = renderPassBeginInfo->framebuffer;
	}

	CommandBufferAccessScopeDesc<kVk> beginInfo{};
	beginInfo.pInheritanceInfo = &inheritInfo;
	// for dynamic rendering, render pass continue just silences validation warnings
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
	beginInfo.level = level;

	return Commands(beginInfo);
}

} // namespace rhi
