#include "../buffer.h"

#include "utils.h"

#include <format>

template <>
Buffer<kVk>::Buffer(Buffer&& other) noexcept
	: DeviceObject(std::forward<Buffer>(other))
	, myBuffer(std::exchange(other.myBuffer, {}))
	, myDesc(std::exchange(other.myDesc, {}))
{}

template <>
Buffer<kVk>::Buffer(
	const std::shared_ptr<Device<kVk>>& device,
	ValueType&& buffer,
	BufferCreateDesc<kVk>&& desc)
	: DeviceObject(
		device,
		[&desc]{ return DeviceObjectCreateDesc{ desc.name.data() }; }(),
		1,
		VK_OBJECT_TYPE_BUFFER,
		reinterpret_cast<uint64_t*>(&std::get<0>(buffer)))
	, myBuffer(std::forward<ValueType>(buffer))
	, myDesc(std::forward<BufferCreateDesc<kVk>>(desc))
{
	// Update the name to point to the DeviceObject's name.
	myDesc.name = GetName();
}

template <>
Buffer<kVk>::Buffer(
	const std::shared_ptr<Device<kVk>>& device, BufferCreateDesc<kVk>&& desc)
	: Buffer(
		device,
		std::tuple_cat(
			CreateBuffer(
				device->GetAllocator(),
				desc.size,
				desc.usageFlags,
				desc.memoryFlags,
				desc.name.data()),
			std::make_tuple(std::optional<TimelineCallback>())),
		std::forward<BufferCreateDesc<kVk>>(desc))
{}

template <>
Buffer<kVk>::Buffer(
	const std::shared_ptr<Device<kVk>>& device,
	CommandBufferHandle<kVk> cmd,
	std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>, BufferCreateDesc<kVk>>&& initialData)
	: Buffer(
		device,
		std::tuple_cat(
			[&cmd, &device, &initialData]
			{
				return CreateBuffer(
					cmd,
					device->GetAllocator(),
					std::get<0>(initialData),
					std::get<2>(initialData).size,
					std::get<2>(initialData).usageFlags,
					std::get<2>(initialData).memoryFlags,
					std::get<2>(initialData).name.data());
			}(),
			std::make_tuple(TimelineCallback(
				[allocator = device->GetAllocator(), buffer = std::get<0>(initialData), memory = std::get<1>(initialData)](uint64_t)
				{
					vmaDestroyBuffer(allocator, buffer, memory);
				}))),
		std::forward<BufferCreateDesc<kVk>>(std::get<2>(initialData)))
{}

template <>
Buffer<kVk>::Buffer(
	const std::shared_ptr<Device<kVk>>& device,
	CommandBufferHandle<kVk> cmd,
	BufferCreateDesc<kVk>&& desc,
	const void* initialData)
	: Buffer(
		device,
		cmd,
		std::tuple_cat(
			CreateStagingBuffer(
				device->GetAllocator(),
				initialData,
				desc.size,
				desc.name.data()),
			std::make_tuple(std::forward<BufferCreateDesc<kVk>>(desc))))
{}

template <>
Buffer<kVk>::~Buffer()
{
	if (BufferHandle<kVk> buffer = *this)
		vmaDestroyBuffer(InternalGetDevice()->GetAllocator(), buffer, GetMemory());
}

template <>
Buffer<kVk>& Buffer<kVk>::operator=(Buffer&& other) noexcept
{
	DeviceObject::operator=(std::forward<Buffer>(other));
	myBuffer = std::exchange(other.myBuffer, {});
	myDesc = std::exchange(other.myDesc, {});
	return *this;
}

template <>
void Buffer<kVk>::Swap(Buffer& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myBuffer, rhs.myBuffer);
	std::swap(myDesc, rhs.myDesc);
}

template <>
BufferView<kVk>::BufferView(BufferView&& other) noexcept
	: DeviceObject(std::forward<BufferView>(other))
	, myView(std::exchange(other.myView, {}))
{}

template <>
BufferView<kVk>::BufferView(
	const std::shared_ptr<Device<kVk>>& device, BufferViewHandle<kVk>&& view)
	: DeviceObject(
		  device,
		  {"_View"},
		  1,
		  VK_OBJECT_TYPE_BUFFER_VIEW,
		  reinterpret_cast<uint64_t*>(&view))
	, myView(std::forward<BufferViewHandle<kVk>>(view))
{}

template <>
BufferView<kVk>::BufferView(
	const std::shared_ptr<Device<kVk>>& device,
	const Buffer<kVk>& buffer,
	Format<kVk> format,
	DeviceSize<kVk> offset,
	DeviceSize<kVk> range)
	: BufferView<kVk>(
		  device,
		  [&device, &buffer, format, offset, range]
		  {
				VkBufferViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
				viewInfo.flags = 0; // "reserved for future use"
				viewInfo.buffer = buffer;
				viewInfo.format = format;
				viewInfo.offset = offset;
				viewInfo.range = range;

				VkBufferView outBufferView;
				VK_CHECK(vkCreateBufferView(*device, &viewInfo, &device->GetInstance()->GetHostAllocationCallbacks(), &outBufferView));

				return outBufferView;
		  }())
{}

template <>
BufferView<kVk>::~BufferView()
{
	if (BufferViewHandle<kVk> view = *this)
		vkDestroyBufferView(*InternalGetDevice(), view, &InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
BufferView<kVk>& BufferView<kVk>::operator=(BufferView&& other) noexcept
{
	DeviceObject::operator=(std::forward<BufferView>(other));
	myView = std::exchange(other.myView, {});
	return *this;
}

template <>
void BufferView<kVk>::Swap(BufferView& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myView, rhs.myView);
}
