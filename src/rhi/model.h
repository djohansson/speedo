#pragma once

#include <rhi/deviceobject.h>
#include <rhi/buffer.h>

#include <gfx/bounds.h>
#include <gfx/vertex.h>

#include <array>
#include <tuple>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
class Model;

template <GraphicsApi G>
struct ModelCreateDesc final : DeviceObjectCreateDesc<G>
{
	Bounds3f bounds;
	uint32_t indexCount = 0;
	uint32_t vertexCount = 0;
	std::vector<VertexInputAttributeDescription<G>> attributes;

	// see DeviceObjectCreateDesc::serialize for why this is needed
	constexpr static auto serialize(auto& archive, auto& self)//NOLINT(readability-identifier-naming)
	{
		using SelfType = std::remove_reference_t<decltype(self)>;
		using BaseType = std::conditional_t<std::is_const_v<SelfType>, const DeviceObjectCreateDesc<G>, DeviceObjectCreateDesc<G>>;
		return archive(
			static_cast<BaseType&>(self),
			self.bounds,
			self.indexCount,
			self.vertexCount,
			self.attributes);
	}
};

template <GraphicsApi G>
struct ObjectTraits<Model<G>>
{
	using CreateDescType = ModelCreateDesc<G>;
};

template <GraphicsApi G>
class Model final : public DeviceObject<Model<G>>
{
public:
	using SuperType = DeviceObject<Model<G>>;
	using CreateDescType = ObjectTraits<Model<G>>::CreateDescType;

	constexpr Model() noexcept = default;
	Model(Model&& other) noexcept = default;
	// todo: load std::spans of vertex and index data instead of loading from file
	Model( // loads a file into a buffer and creates a new model from it. buffer gets garbage collected when finished copying.
		CreateDescType&& desc,
		std::string_view modelFile,
		CommandBufferHandle<G> cmd,
		std::array<core::TaskCreateInfo<void>, 2>& timelineCallbacksOut,
		std::atomic_uint8_t& progressOut);

	[[maybe_unused]] Model& operator=(Model&& other) noexcept = default;

	void Swap(Model& rhs) noexcept;
	friend void Swap(Model& lhs, Model& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetBindings() const noexcept { return myBindings; }
	[[nodiscard]] const auto& GetIndexBuffer() const noexcept { return myIndexBuffer; }
	[[nodiscard]] const auto& GetVertexBuffer() const noexcept { return myVertexBuffer; }

	[[nodiscard]] static std::tuple<BufferHandle<G>, BufferHandle<G>, core::Future<core::Future<void>>>
	LoadModel(std::string_view filePath, std::atomic_uint8_t& progress);

private:
	Model(
		std::tuple<
			BufferHandle<G>,
			AllocationHandle<G>,
			BufferHandle<G>,
			AllocationHandle<G>,
			CreateDescType>&& initialDataAndDesc,
		CommandBufferHandle<G> cmd,
		std::array<core::TaskCreateInfo<void>, 2>& timelineCallbacksOut);

	Buffer<G> myIndexBuffer;
	Buffer<G> myVertexBuffer;
	std::vector<VertexInputBindingDescription<G>> myBindings;
};

} // namespace rhi
