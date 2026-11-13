// the Metal backend's buffers: an MTL::Buffer, in shared storage where the host sees it (unified memory: no staging
// needed to fill it), else private; no allocation of its own (the device's, or later a heap's)
#include <rhi/metal/backend.h>

#include <rhi/buffer.h>
#include <rhi/memory.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Buffer<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Buffer<kMetal>);

namespace metal
{

namespace
{

// the live buffers sorted by their gpu address (the address, the length, the buffer)
struct BufferRange
{
	uint64_t address = 0;
	uint64_t length = 0;
	MTL::Buffer* buffer = nullptr;
};
std::mutex gBuffersMutex;
std::vector<BufferRange> gBuffers;

} // namespace

void RegisterBuffer(MTL::Buffer* buffer)
{
	const BufferRange range{.address = buffer->gpuAddress(), .length = buffer->length(), .buffer = buffer};
	std::scoped_lock lock{gBuffersMutex};
	gBuffers.insert(std::ranges::upper_bound(gBuffers, range.address, {}, &BufferRange::address), range);
}

void UnregisterBuffer(MTL::Buffer* buffer) noexcept
{
	std::scoped_lock lock{gBuffersMutex};
	if (auto it = std::ranges::find(gBuffers, buffer, &BufferRange::buffer); it != gBuffers.end())
		gBuffers.erase(it);
}

std::pair<MTL::Buffer*, uint64_t> FindBuffer(uint64_t address)
{
	std::scoped_lock lock{gBuffersMutex};
	auto it = std::ranges::upper_bound(gBuffers, address, {}, &BufferRange::address);
	ENSUREF(
		it != gBuffers.begin() && address < std::prev(it)->address + std::prev(it)->length,
		"no Metal buffer at gpu address {:#x}",
		address);
	--it;
	return {it->buffer, address - it->address};
}

} // namespace metal

template <>
Buffer<kMetal>::Buffer(CreateDescType&& desc, ValueType&& buffer)
	: DeviceObject<Buffer<kMetal>>(std::forward<CreateDescType>(desc))
	, myBuffer(std::forward<ValueType>(buffer))
{
	if (std::get<0>(myBuffer) != nullptr)
		metal::RegisterBuffer(std::get<0>(myBuffer));
}

template <>
Buffer<kMetal>::Buffer(CreateDescType&& desc)
	: Buffer(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			ZoneScopedN("Buffer::create");

			const bool hostVisible = (desc.memoryFlags & MemoryProperty::kHostVisible) == MemoryProperty::kHostVisible;
			MTL::ResourceOptions options = hostVisible ? MTL::ResourceStorageModeShared : MTL::ResourceStorageModePrivate;
			MTL::Buffer* buffer = desc.device->newBuffer(desc.size, options);
			ENSUREF(buffer != nullptr, "Failed to create a Metal buffer of {} bytes", desc.size);
			buffer->setLabel(NS::String::string(desc.name.c_str(), NS::UTF8StringEncoding));
			return ValueType{buffer, AllocationHandle<kMetal>{}};
		}())
{}

template <>
void Buffer<kMetal>::Swap(Buffer& rhs) noexcept
{
	DeviceObject<Buffer<kMetal>>::Swap(rhs);
	std::swap(myBuffer, rhs.myBuffer);
}

template <>
Buffer<kMetal>::Buffer(Buffer&& other) noexcept
{
	Swap(other);
}

template <>
Buffer<kMetal>& Buffer<kMetal>::operator=(Buffer&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Buffer<kMetal>::~Buffer()
{
	if (IsValid() && std::get<0>(myBuffer) != nullptr)
	{
		metal::UnregisterBuffer(std::get<0>(myBuffer));
		std::get<0>(myBuffer)->release();
	}
}

template <>
std::span<std::byte> Buffer<kMetal>::Map()
{
	auto* buffer = std::get<0>(myBuffer);
	ENSUREF(buffer->storageMode() == MTL::StorageModeShared, "mapping a buffer the host can't see");
	return {static_cast<std::byte*>(buffer->contents()), static_cast<size_t>(buffer->length())};
}

template <>
void Buffer<kMetal>::Unmap()
{}

// shared storage is coherent: neither host writes nor device writes need making visible (after the wait and the barrier)
template <>
void Buffer<kMetal>::Flush(size_t /*offset*/, size_t /*size*/)
{}

template <>
void Buffer<kMetal>::Invalidate(size_t /*offset*/, size_t /*size*/)
{}

template <>
uint64_t Buffer<kMetal>::GetDeviceAddress() const
{
	return std::get<0>(myBuffer)->gpuAddress();
}

namespace buffer
{

static MTL::ResourceOptions Options(const BufferCreateDesc<kMetal>& desc) noexcept
{
	const bool hostVisible = (desc.memoryFlags & MemoryProperty::kHostVisible) == MemoryProperty::kHostVisible;
	return (hostVisible ? MTL::ResourceStorageModeShared : MTL::ResourceStorageModePrivate) | MTL::ResourceHazardTrackingModeTracked;
}

// a buffer of desc filled from source (size bytes, from its start) on cmd's blit encoder
static MTL::Buffer* CreateCopy(const BufferCreateDesc<kMetal>& desc, metal::CommandBufferObject& cmd, MTL::Buffer* source)
{
	MTL::Buffer* buffer = desc.device->newBuffer(desc.size, Options(desc));
	ENSUREF(buffer != nullptr, "Failed to create a Metal buffer of {} bytes", desc.size);
	buffer->setLabel(NS::String::string(desc.name.c_str(), NS::UTF8StringEncoding));
	cmd.BlitEncoder()->copyFromBuffer(source, 0, buffer, 0, desc.size);
	return buffer;
}

} // namespace buffer

template <>
MemoryRequirements Buffer<kMetal>::GetMemoryRequirements(const Device<kMetal>& device, const CreateDescType& desc)
{
	auto sizeAndAlign = static_cast<MTL::Device*>(device)->heapBufferSizeAndAlign(desc.size, buffer::Options(desc));
	return {.size = sizeAndAlign.size, .alignment = sizeAndAlign.align, .memoryTypeBits = ~0U};
}

template <>
Buffer<kMetal>::Buffer(CreateDescType&& desc, const MemoryBlock<kMetal>& memory, uint64_t offset)
	: Buffer(
		std::forward<CreateDescType>(desc),
		[&desc, &memory, offset]
		{
			MTL::Buffer* buffer = memory.GetAllocation()->heap->newBuffer(desc.size, buffer::Options(desc), offset);
			ENSUREF(buffer != nullptr, "Failed to place a Metal buffer of {} bytes at {} in its heap", desc.size, offset);
			buffer->setLabel(NS::String::string(desc.name.c_str(), NS::UTF8StringEncoding));
			return ValueType{buffer, AllocationHandle<kMetal>{}};
		}())
{}

template <>
Buffer<kMetal> Buffer<kMetal>::CreateStaging(DeviceObjectCreateDesc<kMetal>&& desc, size_t size)
{
	return Buffer(BufferCreateDesc<kMetal>{
		std::forward<DeviceObjectCreateDesc<kMetal>>(desc),
		size,
		BufferUsage::kTransferSource,
		MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});
}

template <>
Buffer<kMetal>::Buffer(
	CreateDescType&& desc,
	std::tuple<BufferHandle<kMetal>, AllocationHandle<kMetal>>&& initialData,
	CommandBufferHandle<kMetal> cmd,
	core::TaskCreateInfo<void>& timelineCallbackOut)
	: Buffer(
		std::forward<CreateDescType>(desc),
		ValueType{buffer::CreateCopy(desc, *cmd, std::get<0>(initialData)), AllocationHandle<kMetal>{}})
{
	// the source released once the copy's commands are done
	timelineCallbackOut = core::CreateTask([source = std::get<0>(initialData)] { source->release(); });
}

template <>
Buffer<kMetal>::Buffer(CreateDescType&& desc, const void* initialData, CommandBufferHandle<kMetal> cmd, core::TaskCreateInfo<void>& timelineCallbackOut)
	: Buffer(
		std::forward<CreateDescType>(desc),
		[&desc, initialData]
		{
			MTL::Buffer* staging = desc.device->newBuffer(initialData, desc.size, MTL::ResourceStorageModeShared);
			ENSUREF(staging != nullptr, "Failed to create a Metal staging buffer of {} bytes", desc.size);
			return std::tuple<BufferHandle<kMetal>, AllocationHandle<kMetal>>{staging, AllocationHandle<kMetal>{}};
		}(),
		cmd,
		timelineCallbackOut)
{}

template <>
Buffer<kMetal>::Buffer(CreateDescType&& desc, Buffer&& staging, CommandBufferHandle<kMetal> cmd, core::TaskCreateInfo<void>& timelineCallbackOut)
	: Buffer(std::forward<CreateDescType>(desc), ValueType{buffer::CreateCopy(desc, *cmd, staging.GetBuffer()), AllocationHandle<kMetal>{}})
{
	timelineCallbackOut = core::CreateTask([staging = std::make_shared<Buffer<kMetal>>(std::move(staging))] {});
}

} // namespace rhi
