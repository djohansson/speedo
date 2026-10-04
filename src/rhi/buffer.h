#pragma once

#include <rhi/deviceobject.h>

#include <core/task.h>

#include <tuple>

namespace rhi
{

template <GraphicsApi G>
class Buffer;

template <GraphicsApi G>
class BufferView;

template <GraphicsApi G>
struct BufferCreateDesc final : DeviceObjectCreateDesc<G>
{
	DeviceSize<G> size{};
	Flags<G> usageFlags{};
	Flags<G> memoryFlags{};
};

template <GraphicsApi G>
struct ObjectTraits<Buffer<G>>
{
	using CreateDescType = BufferCreateDesc<G>;
};

template <GraphicsApi G>
class Buffer final : public DeviceObject<Buffer<G>>
{
public:
	using SuperType = DeviceObject<Buffer<G>>;
	using CreateDescType = ObjectTraits<Buffer<G>>::CreateDescType;
	using ValueType = std::tuple<BufferHandle<G>, AllocationHandle<G>>;

	constexpr Buffer() noexcept = default;
	Buffer(Buffer&& other) noexcept;
	explicit Buffer( // creates uninitialized buffer
		CreateDescType&& desc);
	Buffer( // copies initialData into the target, using a temporary internal staging buffer if needed.
		CreateDescType&& desc,
		const void* initialData,
		CommandBufferHandle<G> cmd,
		core::TaskCreateInfo<void>& timelineCallbackOut);
	Buffer( // takes ownership of provided buffer handle and allocation
		CreateDescType&& desc,
		ValueType&& buffer);
	Buffer( // copies buffer in initialData into the target. initialData buffer gets automatically garbage collected when copy has finished.
		CreateDescType&& desc,
		std::tuple<BufferHandle<G>, AllocationHandle<G>>&& initialData,
		CommandBufferHandle<G> cmd,
		core::TaskCreateInfo<void>& timelineCallbackOut);
	~Buffer();

	[[maybe_unused]] Buffer& operator=(Buffer&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return GetBuffer(); }//NOLINT(google-explicit-constructor)

	void Swap(Buffer& rhs) noexcept;
	friend void Swap(Buffer& lhs, Buffer& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetBuffer() const noexcept { return std::get<0>(myBuffer); }
	[[nodiscard]] const auto& GetMemory() const noexcept { return std::get<1>(myBuffer); }

private:
	ValueType myBuffer{};
};

template <GraphicsApi G>
struct BufferViewCreateDesc final : DeviceObjectCreateDesc<G>
{
	Format<G> format{};
	DeviceSize<G> offset{};
	DeviceSize<G> range{};
};

template <GraphicsApi G>
struct ObjectTraits<BufferView<G>>
{
	using CreateDescType = BufferViewCreateDesc<G>;
};

template <GraphicsApi G>
class BufferView final : public DeviceObject<BufferView<G>>
{
public:
	using SuperType = DeviceObject<BufferView<G>>;
	using CreateDescType = ObjectTraits<BufferView<G>>::CreateDescType;

	constexpr BufferView() noexcept = default;
	BufferView(BufferView&& other) noexcept;
	BufferView( // creates a view from buffer
		CreateDescType&& desc,
		const Buffer<G>& buffer);
	~BufferView();

	[[maybe_unused]] BufferView& operator=(BufferView&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return GetView(); }//NOLINT(google-explicit-constructor)

	[[nodiscard]] const auto& GetView() const noexcept { return myView; }

	void Swap(BufferView& rhs) noexcept;
	friend void Swap(BufferView& lhs, BufferView& rhs) noexcept { lhs.Swap(rhs); }

private:
	explicit BufferView( // uses provided image view
		CreateDescType&& desc,
		BufferViewHandle<G>&& view);

	BufferViewHandle<G> myView{};
};

} // namespace rhi
