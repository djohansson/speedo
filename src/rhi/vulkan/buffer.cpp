#include <rhi/buffer.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhiapplication.h>
#include <rhi/vulkan/utils.h>

#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Buffer<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Buffer<kVk>);
IMPLEMENT_OBJECT_GETINSTANCE(BufferView<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(BufferView<kVk>);

template <>
void Buffer<kVk>::Swap(Buffer& rhs) noexcept
{
	DeviceObject<Buffer<kVk>>::Swap(rhs);
	std::swap(myBuffer, rhs.myBuffer);
}

template <>
Buffer<kVk>::Buffer(Buffer&& other) noexcept
{
	Swap(other);
}

template <>
Buffer<kVk>::Buffer(
	CreateDescType&& desc,
	ValueType&& buffer)
	: DeviceObject(std::forward<CreateDescType>(desc))
	, myBuffer(std::forward<ValueType>(buffer))
{}

template <>
Buffer<kVk>::Buffer(
	CreateDescType&& desc)
	: Buffer(
		std::forward<CreateDescType>(desc),
		CreateBuffer(
			GetDevice(desc.device).GetAllocator(),
			desc.size,
			desc.usageFlags,
			desc.memoryFlags,
			nullptr))
{}

template <>
Buffer<kVk>::Buffer(
	CreateDescType&& desc,
	std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>>&& initialData,
	CommandBufferHandle<kVk> cmd,
	core::TaskCreateInfo<void>& timelineCallbackOut)
	: Buffer(
		std::forward<CreateDescType>(desc),
		CreateBuffer(
			cmd,
			GetDevice(desc.device).GetAllocator(),
			std::get<0>(initialData),
			desc.size,
			desc.usageFlags,
			desc.memoryFlags,
			nullptr))
{
	timelineCallbackOut = core::CreateTask(
		[allocator = GetDevice().GetAllocator(), buffer = std::get<0>(initialData), memory = std::get<1>(initialData)]{
			DestroyBuffer(allocator, buffer, memory);
		});
}

template <>
Buffer<kVk>::Buffer(
	CreateDescType&& desc,
	const void* initialData,
	CommandBufferHandle<kVk> cmd,
	core::TaskCreateInfo<void>& timelineCallbackOut)
	: Buffer(
		std::forward<CreateDescType>(desc),
		CreateStagingBuffer(
			GetDevice(desc.device).GetAllocator(),
			initialData,
			desc.size,
			nullptr),
		cmd,
		timelineCallbackOut)
{}

template <>
Buffer<kVk>::~Buffer()
{
	if (IsValid())
		DestroyBuffer(
			GetDevice().GetAllocator(),
			GetBuffer(),
			GetMemory());
}

template <>
Buffer<kVk>& Buffer<kVk>::operator=(Buffer&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
void BufferView<kVk>::Swap(BufferView& rhs) noexcept
{
	DeviceObject<BufferView<kVk>>::Swap(rhs);
	std::swap(myView, rhs.myView);
}

template <>
BufferView<kVk>::BufferView(BufferView&& other) noexcept
{
	Swap(other);
}

template <>
BufferView<kVk>::BufferView(
	CreateDescType&& desc,
	BufferViewHandle<kVk>&& view)
	: DeviceObject<BufferView<kVk>>(std::forward<CreateDescType>(desc))
	, myView(std::forward<BufferViewHandle<kVk>>(view))
{}

template <>
BufferView<kVk>::BufferView(
	CreateDescType&& desc,
	const Buffer<kVk>& buffer)
	: BufferView<kVk>(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&buffer, &desc, this]
		{
			VkBufferViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
			viewInfo.flags = 0; // "reserved for future use"
			viewInfo.buffer = buffer;
			viewInfo.format = desc.format;
			viewInfo.offset = desc.offset;
			viewInfo.range = desc.range;

			VkBufferView outBufferView;
			VK_CHECK(vkCreateBufferView(desc.device, &viewInfo, &GetInstance().GetHostAllocationCallbacks(), &outBufferView));

			Track(desc.device, VK_OBJECT_TYPE_BUFFER_VIEW, outBufferView, GetDebugName(desc));

			return outBufferView;
		}())
{}

template <>
BufferView<kVk>::~BufferView()
{
	if (!IsValid())
		return;

	Untrack(VK_OBJECT_TYPE_BUFFER_VIEW, myView);
	vkDestroyBufferView(
		GetDevice(),
		myView,
		&GetDevice().GetInstance().GetHostAllocationCallbacks());
}

template <>
BufferView<kVk>& BufferView<kVk>::operator=(BufferView&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
