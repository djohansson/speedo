#include "../command.h"

#include "utils.h"

#include <format>

namespace commandbufferarray
{

static auto
CreateArray(const std::shared_ptr<Device<kVk>>& device, const CommandBufferArrayCreateDesc<kVk>& desc)
{
	ZoneScopedN("commandbufferarray::createArray");

	std::array<CommandBufferHandle<kVk>, CommandBufferArray<kVk>::Capacity()> outArray;

	{
		ZoneScopedN("commandbufferarray::createArray::vkAllocateCommandBuffers");

		VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cmdInfo.commandPool = desc.pool;
		cmdInfo.level = desc.level == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
		cmdInfo.commandBufferCount = CommandBufferArray<kVk>::Capacity();
		VK_ENSURE(vkAllocateCommandBuffers(*device, &cmdInfo, outArray.data()));
	}

	return outArray;
}

} // namespace commandbufferarray

template <>
CommandBufferArray<kVk>::CommandBufferArray(
	const std::shared_ptr<Device<kVk>>& device,
	std::tuple<
		CommandBufferArrayCreateDesc<kVk>,
		std::array<CommandBufferHandle<kVk>, kCommandBufferCount>>&& descAndData)
	: DeviceObject(
		  device,
		  {"_CommandBufferArray"},
		  kCommandBufferCount,
		  VK_OBJECT_TYPE_COMMAND_BUFFER,
		  reinterpret_cast<uint64_t*>(std::get<1>(descAndData).data()),
		  uuids::uuid_system_generator{}())
	, myDesc(std::forward<CommandBufferArrayCreateDesc<kVk>>(std::get<0>(descAndData)))
	, myArray(std::forward<std::array<CommandBufferHandle<kVk>, kCommandBufferCount>>(
		  std::get<1>(descAndData)))
{}

template <>
CommandBufferArray<kVk>::CommandBufferArray(
	const std::shared_ptr<Device<kVk>>& device, CommandBufferArrayCreateDesc<kVk>&& desc)
	: CommandBufferArray(
		  device,
		  std::make_tuple(
			  std::forward<CommandBufferArrayCreateDesc<kVk>>(desc),
			  commandbufferarray::CreateArray(device, desc)))
{}

template <>
CommandBufferArray<kVk>::CommandBufferArray(CommandBufferArray&& other) noexcept
	: DeviceObject(std::forward<CommandBufferArray>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myBits(other.myBits)
{
	std::swap(myArray, other.myArray);
}

template <>
CommandBufferArray<kVk>::~CommandBufferArray()
{
	ZoneScopedN("~CommandBufferArray()");

	if (IsValid())
	{
		ZoneScopedN("~CommandBufferArray()::vkFreeCommandBuffers");

		vkFreeCommandBuffers(
			*InternalGetDevice(), myDesc.pool, kCommandBufferCount, myArray.data());
	}
}

template <>
CommandBufferArray<kVk>& CommandBufferArray<kVk>::operator=(CommandBufferArray&& other) noexcept
{
	DeviceObject::operator=(std::forward<CommandBufferArray>(other));
	myDesc = std::exchange(other.myDesc, {});
	std::swap(myArray, other.myArray);
	myBits = other.myBits;
	return *this;
}

template <>
void CommandBufferArray<kVk>::Swap(CommandBufferArray& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myArray, rhs.myArray);
	std::swap(myBits, rhs.myBits);
}

template <>
void CommandBufferArray<kVk>::Reset()
{
	ZoneScopedN("CommandBufferArray::reset");

	ASSERT(!RecordingFlags());
	ASSERT(Head() < kCommandBufferCount);

	if (GetDesc().useBufferReset)
	{
		for (uint32_t i = 0UL; i < Head(); i++)
		{
			ZoneScopedN("CommandBufferArray::reset::vkResetCommandBuffer");

			VK_ENSURE(
				vkResetCommandBuffer(myArray[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));
		}
	}

	myBits = {0, 0};
}

template <>
uint8_t CommandBufferArray<kVk>::Begin(const CommandBufferBeginInfo<kVk>& beginInfo)
{
	ZoneScopedN("CommandBufferArray::begin");

	ASSERT(!Recording(myBits.head));
	ASSERT(!Full());

	VK_ENSURE(vkBeginCommandBuffer(myArray[myBits.head], &beginInfo));

	myBits.recordingFlags |= (1 << myBits.head);

	return (myBits.head++);
}

template <>
void CommandBufferArray<kVk>::End(uint8_t index)
{
	ZoneScopedN("CommandBufferArray::end");

	ASSERT(Recording(index));

	myBits.recordingFlags &= ~(1 << index);

	VK_ENSURE(vkEndCommandBuffer(myArray[index]));
}

template <>
CommandPool<kVk>::CommandPool(
	const std::shared_ptr<Device<kVk>>& device,
	std::tuple<CommandPoolCreateDesc<kVk>, CommandPoolHandle<kVk>>&& descAndData)
	: DeviceObject(
		  device,
		  {},
		  1,
		  VK_OBJECT_TYPE_COMMAND_POOL,
		  reinterpret_cast<uint64_t*>(&std::get<1>(descAndData)),
		  uuids::uuid_system_generator{}())
	, myDesc(std::forward<CommandPoolCreateDesc<kVk>>(std::get<0>(descAndData)))
	, myPool(std::forward<CommandPoolHandle<kVk>>(std::get<1>(descAndData)))
	, myPendingCommands(myDesc.levelCount + 1)
	, mySubmittedCommands(myDesc.levelCount + 1)
	, myFreeCommands(myDesc.levelCount + 1)
	, myRecordingCommands(myDesc.levelCount + 1)
{}

template <>
CommandPool<kVk>::CommandPool(
	const std::shared_ptr<Device<kVk>>& device, CommandPoolCreateDesc<kVk>&& desc)
	: CommandPool(
		  device,
		  std::make_tuple(
			  std::forward<CommandPoolCreateDesc<kVk>>(desc),
			  [&device, &desc]
			  {
					VkCommandPoolCreateInfo cmdPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
					cmdPoolInfo.flags = desc.flags;
					cmdPoolInfo.queueFamilyIndex = desc.queueFamilyIndex;

					VkCommandPool outPool;
					VK_ENSURE(vkCreateCommandPool(
						*device,
						&cmdPoolInfo,
						&device->GetInstance()->GetHostAllocationCallbacks(),
						&outPool));

					return outPool;
				}()))
{}

template <>
CommandPool<kVk>::CommandPool(CommandPool&& other) noexcept
	: DeviceObject(std::forward<CommandPool>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myPendingCommands(std::exchange(other.myPendingCommands, {}))
	, mySubmittedCommands(std::exchange(other.mySubmittedCommands, {}))
	, myFreeCommands(std::exchange(other.myFreeCommands, {}))
	, myRecordingCommands(std::exchange(other.myRecordingCommands, {}))
{
	std::swap(myPool, other.myPool);
}

template <>
CommandPool<kVk>::~CommandPool()
{
	myPendingCommands.clear();
	mySubmittedCommands.clear();
	myFreeCommands.clear();
	myRecordingCommands.clear();

	if (myPool != nullptr)
		vkDestroyCommandPool(
			*InternalGetDevice(),
			myPool,
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
CommandPool<kVk>& CommandPool<kVk>::operator=(CommandPool&& other) noexcept
{
	DeviceObject::operator=(std::forward<CommandPool>(other));
	myDesc = std::exchange(other.myDesc, {});
	std::swap(myPool, other.myPool);
	myPendingCommands = std::exchange(other.myPendingCommands, {});
	mySubmittedCommands = std::exchange(other.mySubmittedCommands, {});
	myFreeCommands = std::exchange(other.myFreeCommands, {});
	myRecordingCommands = std::exchange(other.myRecordingCommands, {});
	return *this;
}

template <>
void CommandPool<kVk>::Swap(CommandPool& other) noexcept
{
	DeviceObject::Swap(other);
	std::swap(myDesc, other.myDesc);
	std::swap(myPool, other.myPool);
	std::swap(myPendingCommands, other.myPendingCommands);
	std::swap(mySubmittedCommands, other.mySubmittedCommands);
	std::swap(myFreeCommands, other.myFreeCommands);
	std::swap(myRecordingCommands, other.myRecordingCommands);
}

template <>
void CommandPool<kVk>::Reset()
{
	ZoneScopedN("CommandPool::reset");

	if ((myDesc.flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) != 0U)
	{
		ZoneScopedN("CommandPool::reset::vkResetCommandPool");

		VK_ENSURE(vkResetCommandPool(
			*InternalGetDevice(), myPool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT));
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
				InternalGetDevice(), CommandBufferArrayCreateDesc<kVk>{*this, level}),
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
		&std::get<0>(myPendingCommands[beginInfo.level].back()), beginInfo));
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
	: CommandBufferBeginInfo<
		  kVk>{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &inheritance}
	, inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}
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
			ASSERT(pInheritanceInfo);
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

