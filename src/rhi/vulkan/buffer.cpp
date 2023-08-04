#include "../buffer.h"

#include "utils.h"

#include <stb_sprintf.h>

template <>
Buffer<Vk>::Buffer(Buffer&& other) noexcept
	: DeviceObject(std::forward<Buffer>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myBuffer(std::exchange(other.myBuffer, {}))
{}

template <>
Buffer<Vk>::Buffer(
	const std::shared_ptr<Device<Vk>>& device,
	BufferCreateDesc<Vk>&& desc,
	ValueType&& buffer)
	: DeviceObject(
		  device,
		  {"_Buffer"},
		  1,
		  VK_OBJECT_TYPE_BUFFER,
		  reinterpret_cast<uint64_t*>(&std::get<0>(buffer)))
	, myDesc(std::forward<BufferCreateDesc<Vk>>(desc))
	, myBuffer(std::forward<ValueType>(buffer))
{}

template <>
Buffer<Vk>::Buffer(
	const std::shared_ptr<Device<Vk>>& device, BufferCreateDesc<Vk>&& desc)
	: Buffer(
		  device,
		  std::forward<BufferCreateDesc<Vk>>(desc),
		  createBuffer(
			  device->getAllocator(),
			  desc.size,
			  desc.usageFlags,
			  desc.memoryFlags,
			  "todo_insert_proper_name"))
{}

template <>
Buffer<Vk>::Buffer(
	const std::shared_ptr<Device<Vk>>& device,
	CommandPoolContext<Vk>& commandContext,
	std::tuple<BufferCreateDesc<Vk>, BufferHandle<Vk>, AllocationHandle<Vk>>&& descAndInitialData)
	: Buffer(
		  device,
		  std::forward<BufferCreateDesc<Vk>>(std::get<0>(descAndInitialData)),
		  createBuffer(
			  commandContext.commands(),
			  device->getAllocator(),
			  std::get<1>(descAndInitialData),
			  std::get<0>(descAndInitialData).size,
			  std::get<0>(descAndInitialData).usageFlags,
			  std::get<0>(descAndInitialData).memoryFlags,
			  "todo_insert_proper_name"))
{
	commandContext.addCommandsFinishedCallback(
		[allocator = device->getAllocator(), descAndInitialData](uint64_t)
		{
			vmaDestroyBuffer(
				allocator,
				std::get<1>(descAndInitialData),
				std::get<2>(descAndInitialData));
		});
}

template <>
Buffer<Vk>::Buffer(
	const std::shared_ptr<Device<Vk>>& device,
	CommandPoolContext<Vk>& commandContext,
	BufferCreateDesc<Vk>&& desc,
	const void* initialData)
	: Buffer(
		  device,
		  commandContext,
		  std::tuple_cat(
			  std::make_tuple(std::forward<BufferCreateDesc<Vk>>(desc)),
			  createStagingBuffer(
				  device->getAllocator(),
				  initialData,
				  desc.size,
				  "todo_insert_proper_name")))
{}

template <>
Buffer<Vk>::~Buffer()
{
	if (BufferHandle<Vk> buffer = *this)
		getDevice()->addTimelineCallback(
			[allocator = getDevice()->getAllocator(),
			 buffer,
			 bufferMemory = getBufferMemory()](uint64_t)
			{ vmaDestroyBuffer(allocator, buffer, bufferMemory); });
}

template <>
Buffer<Vk>& Buffer<Vk>::operator=(Buffer&& other) noexcept
{
	DeviceObject::operator=(std::forward<Buffer>(other));
	myDesc = std::exchange(other.myDesc, {});
	myBuffer = std::exchange(other.myBuffer, {});
	return *this;
}

template <>
void Buffer<Vk>::swap(Buffer& rhs) noexcept
{
	DeviceObject::swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myBuffer, rhs.myBuffer);
}

template <>
BufferView<Vk>::BufferView(BufferView&& other) noexcept
	: DeviceObject(std::forward<BufferView>(other))
	, myView(std::exchange(other.myView, {}))
{}

template <>
BufferView<Vk>::BufferView(
	const std::shared_ptr<Device<Vk>>& device, BufferViewHandle<Vk>&& view)
	: DeviceObject(
		  device,
		  {"_View"},
		  1,
		  VK_OBJECT_TYPE_BUFFER_VIEW,
		  reinterpret_cast<uint64_t*>(&view))
	, myView(std::forward<BufferViewHandle<Vk>>(view))
{}

template <>
BufferView<Vk>::BufferView(
	const std::shared_ptr<Device<Vk>>& device,
	const Buffer<Vk>& buffer,
	Format<Vk> format,
	DeviceSize<Vk> offset,
	DeviceSize<Vk> range)
	: BufferView<Vk>(
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
				VK_CHECK(vkCreateBufferView(*device, &viewInfo, &device->getInstance()->getHostAllocationCallbacks(), &outBufferView));

				return outBufferView;
		  }())
{}

template <>
BufferView<Vk>::~BufferView()
{
	if (BufferViewHandle<Vk> view = *this)
		getDevice()->addTimelineCallback(
			[device = static_cast<DeviceHandle<Vk>>(*getDevice()), callbacks = &getDevice()->getInstance()->getHostAllocationCallbacks(), view](uint64_t)
			{ vkDestroyBufferView(device, view, callbacks); });
}

template <>
BufferView<Vk>& BufferView<Vk>::operator=(BufferView&& other) noexcept
{
	DeviceObject::operator=(std::forward<BufferView>(other));
	myView = std::exchange(other.myView, {});
	return *this;
}

template <>
void BufferView<Vk>::swap(BufferView& rhs) noexcept
{
	DeviceObject::swap(rhs);
	std::swap(myView, rhs.myView);
}
