#pragma once

#include <core/task.h>
#include <rhi/buffer.h>
#include <rhi/deviceobject.h>
#include <rhi/memory.h>

#include <array>
#include <optional>
#include <tuple>

namespace rhi
{

template <GraphicsApi G>
struct ImageMipLevelDesc
{
	Extent2d extent{};
	uint32_t size = 0;
	uint32_t offset = 0;
};

template <GraphicsApi G>
struct ImageCreateDesc final : DeviceObjectCreateDesc<G>
{
	std::vector<ImageMipLevelDesc<G>> mipLevels;
	Format format{};
	ImageTiling tiling{};
	ImageUsage usageFlags{};
	MemoryProperty memoryFlags{};
	ImageAspect imageAspectFlags{};
	ImageLayout layout{};

	// see DeviceObjectCreateDesc::serialize for why this is needed
	constexpr static auto serialize(auto& archive, auto& self)//NOLINT(readability-identifier-naming)
	{
		using SelfType = std::remove_reference_t<decltype(self)>;
		using BaseType = std::conditional_t<std::is_const_v<SelfType>, const DeviceObjectCreateDesc<G>, DeviceObjectCreateDesc<G>>;
		return archive(
			static_cast<BaseType&>(self),
			self.mipLevels,
			self.format,
			self.tiling,
			self.usageFlags,
			self.memoryFlags,
			self.imageAspectFlags,
			self.layout);
	}
};

template <GraphicsApi G>
class Image;

template <GraphicsApi G>
class ImageView;

template <GraphicsApi G>
struct ObjectTraits<Image<G>>
{
	using CreateDescType = ImageCreateDesc<G>;
};

template <GraphicsApi G>
class Image final : public DeviceObject<Image<G>>
{
public:
	using SuperType = DeviceObject<Image<G>>;
	using CreateDescType = ObjectTraits<Image<G>>::CreateDescType;
	using ValueType = std::tuple<ImageHandle<G>, AllocationHandle<G>>;

	constexpr Image() noexcept = default;
	Image(Image&& other) noexcept;
	explicit Image( // creates uninitialized image
		CreateDescType&& desc);
	Image( // creates an uninitialized image placed at offset in memory (see MemoryBlock), which must outlive it. it
		   // aliases whatever else is placed over the same bytes: see Discard.
		CreateDescType&& desc,
		const MemoryBlock<G>& memory,
		uint64_t offset);
	Image( // copies a staging buffer (see Buffer::CreateStaging) into the target, mip level by mip level as desc lays them
		   // out, and releases the staging buffer from timlineCallbackOut. leaves the image in kTransferDestination.
		CreateDescType&& desc,
		Buffer<G>&& staging,
		CommandBufferHandle<G> cmd,
		core::TaskCreateInfo<void>& timlineCallbackOut);
	Image( // copies initialData into the target, using a temporary internal staging buffer if needed.
		CreateDescType&& desc,
		CommandBufferHandle<G> cmd,
		const void* initialData,
		size_t initialDataSize,
		core::TaskCreateInfo<void>& timlineCallbackOut);
	~Image();

	[[maybe_unused]] Image& operator=(Image&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return std::get<0>(myImage); }//NOLINT(google-explicit-constructor)

	void Swap(Image& rhs) noexcept;
	friend void Swap(Image& lhs, Image& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] auto GetMemory() const noexcept { return std::get<1>(myImage); }

	// what an image of desc needs to be placed in a MemoryBlock
	[[nodiscard]] static MemoryRequirements GetMemoryRequirements(const Device<G>& device, const CreateDescType& desc);

	// forgets the image's contents: its next Transition is from kUndefined, which lets the driver discard them (for an
	// image placed over memory another resource used since, whose bytes its contents no longer are)
	void Discard() noexcept { InternalSetImageLayout(ImageLayout::kUndefined); }

	void Clear(
		CommandBufferHandle<G> cmd,
		const ClearValue& value = {},
		const std::optional<ImageSubresourceRange<G>>& range = std::nullopt);
	void Transition(CommandBufferHandle<G> cmd, ImageLayout layout, ImageAspect aspectFlags = {});
	// blits source's level 0 (in kTransferSource) to this image's level 0, scaled (linear) to its size. leaves all of this
	// image's levels in kTransferDestination
	void BlitFrom(CommandBufferHandle<G> cmd, const Image& source);
	// fills levels 1 and up from level 0, each blitted (linear) from the one above, with all levels in
	// kTransferDestination (as BlitFrom leaves them). leaves them in layout
	void GenerateMips(CommandBufferHandle<G> cmd, ImageLayout layout);

private:
	Image( // copies buffer in initialData into the target. initialData buffer gets automatically garbage collected when copy has finished.
		CreateDescType&& desc,
		CommandBufferHandle<G> cmd,
		core::TaskCreateInfo<void>& timlineCallbackOut,
		std::tuple<BufferHandle<G>, AllocationHandle<G>>&& initialData);
	Image( // takes ownership of provided image handle & allocation
		CreateDescType&& desc,
		ValueType&& data);

	template <GraphicsApi GApi>
	friend class RenderImageSet;

	// these methods are not meant to be used except in very special cases
	// such as for instance to update the image layout after a render pass
	// (which implicitly changes the image layout).
	void InternalSetImageLayout(ImageLayout layout) noexcept { this->InternalGetDesc().layout = layout; }
	void InternalSetAspectFlags(ImageAspect aspectFlags) noexcept { this->InternalGetDesc().imageAspectFlags = aspectFlags; }

	ValueType myImage{};
};

template <GraphicsApi G>
struct ImageViewCreateDesc final : DeviceObjectCreateDesc<G>
{
	ImageHandle<G> image{};
	Format format{};
	ImageAspect aspectFlags{};
	uint32_t levelCount = 0; // the mip levels it views, from level 0: 0 for all of the image's
	// where its r, g, b and a come from (e.g. a texture whose channels the shader expects elsewhere)
	std::array<ComponentSwizzle, 4> components{};
};

template <GraphicsApi G>
class ImageView;

template <GraphicsApi G>
struct ObjectTraits<ImageView<G>>
{
	using CreateDescType = ImageViewCreateDesc<G>;
};

template <GraphicsApi G>
class ImageView final : public DeviceObject<ImageView<G>>
{
public:
	using SuperType = DeviceObject<ImageView<G>>;
	using CreateDescType = ObjectTraits<ImageView<G>>::CreateDescType;

	constexpr ImageView() noexcept = default;
	ImageView(ImageView&& other) noexcept;
	explicit ImageView(CreateDescType&& desc);
	~ImageView() final;

	[[maybe_unused]] ImageView& operator=(ImageView&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myView; }//NOLINT(google-explicit-constructor)

	void SetFormat(Format format) noexcept { this->InternalGetDesc().format = format; }
	void SetAspectFlags(Flags<G> aspectFlags) noexcept { this->InternalGetDesc().aspectFlags = aspectFlags; }

	void Swap(ImageView& rhs) noexcept;
	friend void Swap(ImageView& lhs, ImageView& rhs) noexcept { lhs.Swap(rhs); }

private:
	explicit ImageView( // uses provided image view
		CreateDescType&& desc,
		ImageViewHandle<G>&& view);

	ImageViewHandle<G> myView{};
};

} // namespace rhi
