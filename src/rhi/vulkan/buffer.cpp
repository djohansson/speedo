#include "../buffer.h"

#include "utils.h"

#include <format>

template <>
Buffer<kVk>::Buffer(Buffer&& other) noexcept
	: DeviceObject(std::forward<Buffer>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myBuffer(std::exchange(other.myBuffer, {}))
{}

template <>
Buffer<kVk>::Buffer(
	const std::shared_ptr<Device<kVk>>& device,
	BufferCreateDesc<kVk>&& desc,
	ValueType&& buffer)
	: DeviceObject(
		  device,
		  {"_Buffer"},
		  1,
		  VK_OBJECT_TYPE_BUFFER,
		  reinterpret_cast<uint64_t*>(&std::get<0>(buffer)))
	, myDesc(std::forward<BufferCreateDesc<kVk>>(desc))
	, myBuffer(std::forward<ValueType>(buffer))
{}

template <>
Buffer<kVk>::Buffer(
	const std::shared_ptr<Device<kVk>>& device, BufferCreateDesc<kVk>&& desc)
	: Buffer(
		  device,
		  std::forward<BufferCreateDesc<kVk>>(desc),
		  CreateBuffer(
			  device->GetAllocator(),
			  desc.size,
			  desc.usageFlags,
			  desc.memoryFlags,
			  "todo_insert_proper_name"))
{}

template <>
Buffer<kVk>::Buffer(
	const std::shared_ptr<Device<kVk>>& device,
	Queue<kVk>& queue,
	uint64_t timelineValue,
	std::tuple<BufferCreateDesc<kVk>, BufferHandle<kVk>, AllocationHandle<kVk>>&& descAndInitialData)
	: Buffer(
		  device,
		  std::forward<BufferCreateDesc<kVk>>(std::get<0>(descAndInitialData)),
		  CreateBuffer(
			  queue.GetPool().Commands(),
			  device->GetAllocator(),
			  std::get<1>(descAndInitialData),
			  std::get<0>(descAndInitialData).size,
			  std::get<0>(descAndInitialData).usageFlags,
			  std::get<0>(descAndInitialData).memoryFlags,
			  "todo_insert_proper_name"))
{
	queue.AddTimelineCallback(
	{
		timelineValue,
		[allocator = device->GetAllocator(), descAndInitialData](uint64_t)
		{
			vmaDestroyBuffer(
				allocator,
				std::get<1>(descAndInitialData),
				std::get<2>(descAndInitialData));
		}
	});
}

template <>
Buffer<kVk>::Buffer(
	const std::shared_ptr<Device<kVk>>& device,
	Queue<kVk>& queue,
	uint64_t timelineValue,
	BufferCreateDesc<kVk>&& desc,
	const void* initialData)
	: Buffer(
		  device,
		  queue,
		  timelineValue,
		  std::tuple_cat(
			  std::make_tuple(std::forward<BufferCreateDesc<kVk>>(desc)),
			  CreateStagingBuffer(
				  device->GetAllocator(),
				  initialData,
				  desc.size,
				  "todo_insert_proper_name")))
{}

template <>
Buffer<kVk>::~Buffer()
{
	if (BufferHandle<kVk> buffer = *this)
		vmaDestroyBuffer(GetDevice()->GetAllocator(), buffer, GetBufferMemory());
}

template <>
Buffer<kVk>& Buffer<kVk>::operator=(Buffer&& other) noexcept
{
	DeviceObject::operator=(std::forward<Buffer>(other));
	myDesc = std::exchange(other.myDesc, {});
	myBuffer = std::exchange(other.myBuffer, {});
	return *this;
}

template <>
void Buffer<kVk>::Swap(Buffer& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myBuffer, rhs.myBuffer);
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
		vkDestroyBufferView(*GetDevice(), view, &GetDevice()->GetInstance()->GetHostAllocationCallbacks());
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
