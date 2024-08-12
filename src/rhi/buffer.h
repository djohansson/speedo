#pragma once

#include "device.h"
#include "queue.h"

#include <memory>
#include <string>
#include <tuple>

template <GraphicsApi G>
struct BufferCreateDesc
{
	DeviceSize<G> size{};
	Flags<G> usageFlags{};
	Flags<G> memoryFlags{};
	std::string name{};
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
		ValueType&& buffer,
		BufferCreateDesc<G>&& desc);
	Buffer( // copies buffer in initialData into the target. initialData buffer gets automatically garbage collected when copy has finished.
		const std::shared_ptr<Device<G>>& device,
		Queue<G>& queue,
		uint64_t timelineValue,
		std::tuple<BufferHandle<G>, AllocationHandle<G>, BufferCreateDesc<G>>&& initialData);
	~Buffer() override;

	Buffer& operator=(Buffer&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return std::get<0>(myBuffer); }//NOLINT(google-explicit-constructor)

	void Swap(Buffer& rhs) noexcept;
	friend void Swap(Buffer& lhs, Buffer& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] const auto& GetMemory() const noexcept { return std::get<1>(myBuffer); }

private:
	ValueType myBuffer{};
	BufferCreateDesc<G> myDesc{};
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
	[[nodiscard]] operator auto() const noexcept { return myView; }//NOLINT(google-explicit-constructor)

	void Swap(BufferView& rhs) noexcept;
	friend void Swap(BufferView& lhs, BufferView& rhs) noexcept { lhs.Swap(rhs); }

private:
	BufferView( // uses provided image view
		const std::shared_ptr<Device<G>>& device,
		BufferViewHandle<G>&& view);

	BufferViewHandle<G> myView{};
};
