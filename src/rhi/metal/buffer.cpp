// the Metal backend's buffers: an MTL::Buffer, in shared storage where the host sees it (unified memory: no staging
// needed to fill it), else private; no allocation of its own (the device's, or later a heap's)
#include <rhi/metal/backend.h>

#include <rhi/buffer.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <span>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Buffer<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Buffer<kMetal>);

template <>
Buffer<kMetal>::Buffer(CreateDescType&& desc, ValueType&& buffer)
	: DeviceObject<Buffer<kMetal>>(std::forward<CreateDescType>(desc))
	, myBuffer(std::forward<ValueType>(buffer))
{}

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
		std::get<0>(myBuffer)->release();
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

} // namespace rhi
