#pragma once

#include "command.h"
#include "device.h"
#include "types.h"

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
		CommandPoolContext<G>& commandContext,
		BufferCreateDesc<G>&& desc,
		const void* initialData);
	~Buffer();

	Buffer& operator=(Buffer&& other) noexcept;
	operator auto() const noexcept { return std::get<0>(myBuffer); }

	void swap(Buffer& rhs) noexcept;
	friend void swap(Buffer& lhs, Buffer& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const noexcept { return myDesc; }
	const auto& getBufferMemory() const noexcept { return std::get<1>(myBuffer); }

protected:
	Buffer( // copies buffer in descAndInitialData into the target. descAndInitialData buffer gets automatically garbage collected when copy has finished.
		const std::shared_ptr<Device<G>>& device,
		CommandPoolContext<G>& commandContext,
		std::tuple<BufferCreateDesc<G>, BufferHandle<G>, AllocationHandle<G>>&& descAndInitialData);

private:
	Buffer( // takes ownership of provided buffer handle and allocation
		const std::shared_ptr<Device<G>>& device,
		BufferCreateDesc<G>&& desc,
		ValueType&& buffer);

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

	void swap(BufferView& rhs) noexcept;
	friend void swap(BufferView& lhs, BufferView& rhs) noexcept { lhs.swap(rhs); }

private:
	BufferView( // uses provided image view
		const std::shared_ptr<Device<G>>& device,
		BufferViewHandle<G>&& view);

	BufferViewHandle<G> myView{};
};
