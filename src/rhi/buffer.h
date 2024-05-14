#pragma once

#include "device.h"
#include "queue.h"

#include <memory>
#include <tuple>

template <GraphicsApi G>
struct BufferCreateDesc
{
	DeviceSize<G> size{};
	Flags<G> usageFlags{};
	Flags<G> memoryFlags{};
};

template <GraphicsApi G>
class Buffer : public DeviceObject<G>
{
	using ValueType = std::tuple<BufferHandle<G>, AllocationHandle<G>>;

public:
	constexpr Buffer() noexcept = default;
	Buffer(Buffer&& other) noexcept;
	Buffer( // creates uninitialized buffer
		const std::shared_ptr<Device<G>>& device,
		BufferCreateDesc<G>&& desc);
	Buffer( // copies initialData into the target, using a temporary internal staging buffer if needed.
		const std::shared_ptr<Device<G>>& device,
		Queue<G>& queue,
		uint64_t timelineValue,
		BufferCreateDesc<G>&& desc,
		const void* initialData);
	Buffer( // takes ownership of provided buffer handle and allocation
		const std::shared_ptr<Device<G>>& device,
		BufferCreateDesc<G>&& desc,
		ValueType&& buffer);
	Buffer( // copies buffer in descAndInitialData into the target. descAndInitialData buffer gets automatically garbage collected when copy has finished.
		const std::shared_ptr<Device<G>>& device,
		Queue<G>& queue,
		uint64_t timelineValue,
		std::tuple<BufferCreateDesc<G>, BufferHandle<G>, AllocationHandle<G>>&& descAndInitialData);
	~Buffer();

	Buffer& operator=(Buffer&& other) noexcept;
	operator auto() const noexcept { return std::get<0>(myBuffer); }

	void Swap(Buffer& rhs) noexcept;
	friend void Swap(Buffer& lhs, Buffer& rhs) noexcept { lhs.Swap(rhs); }

	const auto& GetDesc() const noexcept { return myDesc; }
	const auto& getBufferMemory() const noexcept { return std::get<1>(myBuffer); }

	BufferCreateDesc<G> myDesc{};
	ValueType myBuffer{};
};

template <GraphicsApi G>
class BufferView : public DeviceObject<G>
{
public:
	constexpr BufferView() noexcept = default;
	BufferView(BufferView&& other) noexcept;
	BufferView( // creates a view from buffer
		const std::shared_ptr<Device<G>>& device,
		const Buffer<G>& buffer,
		Format<G> format,
		DeviceSize<G> offset,
		DeviceSize<G> range);
	~BufferView();

	BufferView& operator=(BufferView&& other) noexcept;
	operator auto() const noexcept { return myView; }

	void Swap(BufferView& rhs) noexcept;
	friend void Swap(BufferView& lhs, BufferView& rhs) noexcept { lhs.Swap(rhs); }

private:
	BufferView( // uses provided image view
		const std::shared_ptr<Device<G>>& device,
		BufferViewHandle<G>&& view);

	BufferViewHandle<G> myView{};
};
