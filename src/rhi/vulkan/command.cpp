#include <rhi/command.h>
#include <rhi/device.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

#include <format>
#include <print>
#include <utility>

#include <vulkan/vulkan_core.h>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(CommandPool<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(CommandPool<kVk>);

namespace commandpool
{

// the kinds of command buffer a pool keeps apart (indices into its lists)
constexpr size_t kPrimary = 0;
constexpr size_t kSecondary = 1;

// command buffers are allocated this many at a time, when a pool has none free of the kind asked for
constexpr uint32_t kAllocationBatch = 8;

} // namespace commandpool

// defined before CommandBuffer's members, which use it (see CommandBuffer::Release)
template <>
void CommandPool<kVk>::Retire(CommandBufferHandle<kVk> handle, bool secondary)
{
	using namespace commandpool;

	ENSURE(myOutstanding > 0);
	myOutstanding--;
	myUsed[secondary ? kSecondary : kPrimary].push_back(handle);
}

template <>
void CommandBuffer<kVk>::End()
{
	ZoneScopedN("CommandBuffer::End");

	ENSURE(myRecording);
	myRecording = false;
	VK_CHECK(vkEndCommandBuffer(myHandle));
}

template <>
CommandBuffer<kVk>::~CommandBuffer()
{
	if (!IsValid())
		return;

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	std::println(stderr, "a command buffer was dropped without being submitted: its commands won't run");
#endif
	(void)Release();
}

template <>
CommandPool<kVk>::CommandPool(CreateDescType&& desc, CommandPoolHandle<kVk>&& pool)
	: DeviceObject<CommandPool<kVk>>(std::forward<CreateDescType>(desc))
	, myPool(std::forward<CommandPoolHandle<kVk>>(pool))
{
	ASSERT(myPool != VK_NULL_HANDLE);
}

template <>
CommandPool<kVk>::CommandPool(CreateDescType&& desc)
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
	std::swap(myFree, other.myFree);
	std::swap(myUsed, other.myUsed);
	std::swap(myOutstanding, other.myOutstanding);
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

	ENSUREF(myOutstanding == 0, "{} command buffers outlive their pool", myOutstanding);

	for (auto* lists : {&myFree, &myUsed})
		for (auto& handles : *lists)
		{
			if (handles.empty())
				continue;
			for (auto* handle : handles)
				Untrack(VK_OBJECT_TYPE_COMMAND_BUFFER, handle);
			vkFreeCommandBuffers(GetDevice(), myPool, static_cast<uint32_t>(handles.size()), handles.data());
		}

	Untrack(VK_OBJECT_TYPE_COMMAND_POOL, myPool);
	vkDestroyCommandPool(GetDevice(), myPool, &GetInstance().GetHostAllocationCallbacks());
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
	ZoneScopedN("CommandPool::Reset");

	ENSUREF(myOutstanding == 0, "resetting a command pool while {} of its command buffers are held", myOutstanding);

	// keep the pool's memory for the next frame: releasing it frees KosmicKrisp's Metal command allocators, and freeing one
	// whose memory Metal hasn't taken back from its command buffers yet aborts (IOGPUMetalDeviceShmemPool's
	// `_allocatedSize == 0` assert), though the fences of everything recorded from it were waited on. it hit one in
	// three runs that loaded DamagedHelmet, then AnimationPointerUVs, with a cold pipeline cache (profile build)
	constexpr bool kUseReleaseResources = false;

	{
		ZoneScopedN("CommandPool::Reset::vkResetCommandPool");

		VK_CHECK(vkResetCommandPool(GetDevice(), myPool, kUseReleaseResources ? VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT : 0));
	}

	for (size_t kind = 0; kind < myUsed.size(); kind++)
	{
		myFree[kind].insert(myFree[kind].end(), myUsed[kind].begin(), myUsed[kind].end());
		myUsed[kind].clear();
	}
}

template <>
CommandBufferHandle<kVk> CommandPool<kVk>::InternalTake(bool secondary)
{
	using namespace commandpool;

	auto& free = myFree[secondary ? kSecondary : kPrimary];
	if (free.empty())
	{
		ZoneScopedN("CommandPool::vkAllocateCommandBuffers");

		std::array<CommandBufferHandle<kVk>, kAllocationBatch> handles{};
		VkCommandBufferAllocateInfo cmdInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cmdInfo.commandPool = myPool;
		cmdInfo.level = secondary ? VK_COMMAND_BUFFER_LEVEL_SECONDARY : VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmdInfo.commandBufferCount = kAllocationBatch;
		VK_CHECK(vkAllocateCommandBuffers(GetDevice(), &cmdInfo, handles.data()));

		auto allocated = myFree[kPrimary].size() + myFree[kSecondary].size() + myUsed[kPrimary].size() + myUsed[kSecondary].size();
		for (size_t handleIt = 0; handleIt < handles.size(); handleIt++)
			Track(
				GetDevice(),
				VK_OBJECT_TYPE_COMMAND_BUFFER,
				handles[handleIt],
				std::format("{} {} {}", GetDebugName(GetDesc()), secondary ? "Secondary" : "Primary", allocated + handleIt));
		free.insert(free.end(), handles.begin(), handles.end());
	}

	auto* handle = free.back();
	free.pop_back();
	myOutstanding++;
	return handle;
}

template <>
CommandBuffer<kVk> CommandPool<kVk>::Begin()
{
	ZoneScopedN("CommandPool::Begin");

	auto* handle = InternalTake(false);
	VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkBeginCommandBuffer(handle, &beginInfo));
	return CommandBuffer<kVk>(*this, handle, false);
}

template <>
CommandBuffer<kVk> CommandPool<kVk>::BeginSecondary(const RenderTargetBeginInfo<kVk>& renderTarget)
{
	ZoneScopedN("CommandPool::BeginSecondary");

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

	auto* handle = InternalTake(true);
	VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	// for dynamic rendering, render pass continue just silences validation warnings
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
	beginInfo.pInheritanceInfo = &inheritInfo;
	VK_CHECK(vkBeginCommandBuffer(handle, &beginInfo));
	return CommandBuffer<kVk>(*this, handle, true);
}

template <>
CommandBufferHandle<kVk> CommandPool<kVk>::Allocate()
{
	auto* handle = InternalTake(false);
	Retire(handle, false);
	return handle;
}

} // namespace rhi
