#pragma once

#include <core/task.h>
#include <rhi/deviceobject.h>

#include <memory>
#include <optional>
#include <string_view>
#include <tuple>

namespace rhi
{

template <GraphicsApi G>
struct ImageMipLevelDesc
{
	Extent2d<G> extent{};
	uint32_t size = 0;
	uint32_t offset = 0;
};

template <GraphicsApi G>
struct ImageCreateDesc final : DeviceObjectCreateDesc<G>
{
	std::vector<ImageMipLevelDesc<G>> mipLevels;
	Format<G> format{};
	ImageTiling<G> tiling{};
	Flags<G> usageFlags{};
	Flags<G> memoryFlags{};
	ImageAspectFlags<G> imageAspectFlags{};
	ImageLayout<G> layout{};

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
	Image( // loads a file into a buffer and creates a new image from it.
		DeviceHandle<G> device,
		CommandBufferHandle<G> cmd,
		std::string_view imageFile,
		std::atomic_uint8_t& progressOut,
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

	void Clear(
		CommandBufferHandle<G> cmd,
		const ClearValue<G>& value = {},
		const std::optional<ImageSubresourceRange<G>>& range = std::nullopt);
	void Transition(CommandBufferHandle<G> cmd, ImageLayout<G> layout, ImageAspectFlags<G> aspectFlags = {});

	// loads and uploads an image (plus a view of it). returns once the upload has completed; the image is left in the
	// layout of the upload, so the caller must transition it before sampling from it.
	[[nodiscard]]
	static std::tuple<std::shared_ptr<Image<G>>, std::shared_ptr<ImageView<G>>>
	LoadImage(DeviceHandle<G> deviceHandle, std::string_view imageFile, std::atomic_uint8_t& progress);

private:
	Image( // copies buffer in initialData into the target. initialData buffer gets automatically garbage collected when copy has finished.
		CommandBufferHandle<G> cmd,
		core::TaskCreateInfo<void>& timlineCallbackOut,
		std::tuple<BufferHandle<G>, AllocationHandle<G>, CreateDescType>&& initialDataAndDesc);
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
	void InternalSetImageLayout(ImageLayout<G> layout) noexcept { this->InternalGetDesc().layout = layout; }
	void InternalSetAspectFlags(ImageAspectFlags<G> aspectFlags) noexcept { this->InternalGetDesc().imageAspectFlags = aspectFlags; }

	ValueType myImage{};
};

template <GraphicsApi G>
struct ImageViewCreateDesc final : DeviceObjectCreateDesc<G>
{
	ImageHandle<G> image{};
	Format<G> format{};
	Flags<G> aspectFlags{};
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

	void SetFormat(Format<G> format) noexcept { this->InternalGetDesc().format = format; }
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
