#pragma once

#include "device.h"
#include "queue.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>

template <GraphicsApi G>
struct ImageMipLevelDesc
{
	Extent2d<G> extent{};
	uint32_t size = 0;
	uint32_t offset = 0;
};

template <GraphicsApi G>
struct ImageCreateDesc
{
	std::vector<ImageMipLevelDesc<G>> mipLevels;
	Format<G> format{};
	ImageTiling<G> tiling{};
	Flags<G> usageFlags{};
	Flags<G> memoryFlags{};
	ImageLayout<G> initialLayout{};
	std::string_view name; // initially points to an arbitrary string, but will be replaced with a string_view to a string stored DeviceObject during construction of Image.
};

template <GraphicsApi G>
class Image : public DeviceObject<G>
{
	using ValueType = std::tuple<ImageHandle<G>, AllocationHandle<G>, ImageLayout<G>>;

public:
	constexpr Image() noexcept = default;
	Image(Image&& other) noexcept;
	Image( // creates uninitialized image
		const std::shared_ptr<Device<G>>& device,
		ImageCreateDesc<G>&& desc);
	Image( // loads a file into a buffer and creates a new image from it.
		const std::shared_ptr<Device<G>>& device,
		TaskCreateInfo<void>& timlineCallbackOut,
		CommandBufferHandle<G> cmd,
		const std::filesystem::path& imageFile,
		std::atomic_uint8_t& progress);
	Image( // copies initialData into the target, using a temporary internal staging buffer if needed.
		const std::shared_ptr<Device<G>>& device,
		TaskCreateInfo<void>& timlineCallbackOut,
		CommandBufferHandle<G> cmd,
		ImageCreateDesc<G>&& desc,
		const void* initialData,
		size_t initialDataSize);
	~Image() override;

	[[maybe_unused]] Image& operator=(Image&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return std::get<0>(myImage); }//NOLINT(google-explicit-constructor)

	void Swap(Image& rhs) noexcept;
	friend void Swap(Image& lhs, Image& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] const auto& GetMemory() const noexcept { return std::get<1>(myImage); }
	[[nodiscard]] const auto& GetLayout() const noexcept { return std::get<2>(myImage); }

	enum class ClearType : uint8_t
	{
		kColor,
		kDepthStencil
	};
	void Clear(
		CommandBufferHandle<G> cmd,
		const ClearValue<G>& value = {},
		ClearType type = ClearType::kColor,
		const std::optional<ImageSubresourceRange<G>>& range = std::nullopt);
	void Transition(CommandBufferHandle<G> cmd, ImageLayout<G> layout);

private:
	Image( // copies buffer in initialData into the target. initialData buffer gets automatically garbage collected when copy has finished.
		const std::shared_ptr<Device<G>>& device,
		TaskCreateInfo<void>& timlineCallbackOut,
		CommandBufferHandle<G> cmd,
		std::tuple<BufferHandle<G>, AllocationHandle<G>, ImageCreateDesc<G>>&& initialData);
	Image( // takes ownership of provided image handle & allocation
		const std::shared_ptr<Device<G>>& device,
		ValueType&& data,
		ImageCreateDesc<G>&& desc);

	template <GraphicsApi GApi>
	friend class RenderImageSet;

	// this method is not meant to be used except in very special cases
	// such as for instance to update the image layout after a render pass
	// (which implicitly changes the image layout).
	void InternalSetImageLayout(ImageLayout<G> layout) noexcept { std::get<2>(myImage) = layout; }

	ValueType myImage{};
	ImageCreateDesc<G> myDesc{};
};

template <GraphicsApi G>
class ImageView : public DeviceObject<G>
{
public:
	constexpr ImageView() noexcept = default;
	ImageView(ImageView&& other) noexcept;
	ImageView( // creates a view from image
		const std::shared_ptr<Device<G>>& device,
		const Image<G>& image,
		Flags<kVk> aspectFlags);
	~ImageView() override;

	[[maybe_unused]] ImageView& operator=(ImageView&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myView; }//NOLINT(google-explicit-constructor)

	void Swap(ImageView& rhs) noexcept;
	friend void Swap(ImageView& lhs, ImageView& rhs) noexcept { lhs.Swap(rhs); }

private:
	ImageView( // uses provided image view
		const std::shared_ptr<Device<G>>& device,
		ImageViewHandle<G>&& view);

	ImageViewHandle<G> myView{};
};

namespace image
{

template <GraphicsApi G>
[[nodiscard]] std::pair<Image<G>, ImageView<G>> LoadImage(
	std::string_view filePath,
	std::atomic_uint8_t& progress,
	std::shared_ptr<Image<G>> oldImage = nullptr,
	std::shared_ptr<ImageView<G>> oldImageView = nullptr);

} // namespace image
