#pragma once

#include "command.h"
#include "device.h"
#include "types.h"

#include <filesystem>
#include <memory>
#include <tuple>

template <GraphicsBackend B>
struct ImageMipLevelDesc
{
	Extent2d<B> extent{};
	uint32_t size = 0;
	uint32_t offset = 0;
};

template <GraphicsBackend B>
struct ImageCreateDesc
{
	std::vector<ImageMipLevelDesc<B>> mipLevels;
	Format<B> format{};
	ImageTiling<B> tiling{};
	Flags<B> usageFlags{};
	Flags<B> memoryFlags{};
	ImageLayout<B> initialLayout{};
};

template <GraphicsBackend B>
class Image : public DeviceObject<B>
{
	using ValueType = std::tuple<ImageHandle<B>, AllocationHandle<B>, ImageLayout<B>>;

public:
	constexpr Image() noexcept = default;
	Image(Image&& other) noexcept;
	Image( // creates uninitialized image
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		ImageCreateDesc<B>&& desc);
	Image( // loads a file into a buffer and creates a new image from it.
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		CommandPoolContext<B>& commandContext,
		const std::filesystem::path& imageFile);
	Image( // copies initialData into the target, using a temporary internal staging buffer if needed.
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		CommandPoolContext<B>& commandContext,
		ImageCreateDesc<B>&& desc,
		const void* initialData,
		size_t initialDataSize);
	~Image();

	Image& operator=(Image&& other) noexcept;
	operator auto() const noexcept { return std::get<0>(myImage); }

	void swap(Image& rhs) noexcept;
	friend void swap(Image& lhs, Image& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const noexcept { return myDesc; }
	const auto& getImageMemory() const noexcept { return std::get<1>(myImage); }
	const auto& getImageLayout() const noexcept { return std::get<2>(myImage); }

	enum class ClearType : uint8_t
	{
		Color,
		DepthStencil
	};
	void clear(
		CommandBufferHandle<B> cmd,
		const ClearValue<B>& value = {},
		ClearType type = ClearType::Color,
		const std::optional<ImageSubresourceRange<B>>& range = std::nullopt);
	void transition(CommandBufferHandle<B> cmd, ImageLayout<B> layout);

private:
	Image( // copies buffer in descAndInitialData into the target. descAndInitialData buffer gets automatically garbage collected when copy has finished.
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		CommandPoolContext<B>& commandContext,
		std::tuple<ImageCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);
	Image( // takes ownership of provided image handle & allocation
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		ImageCreateDesc<B>&& desc,
		ValueType&& data);

	template <GraphicsBackend GB>
	friend class RenderImageSet;

	// this method is not meant to be used except in very special cases
	// such as for instance to update the image layout after a render pass
	// (which implicitly changes the image layout).
	void setImageLayout(ImageLayout<B> layout) noexcept { std::get<2>(myImage) = layout; }

	ImageCreateDesc<B> myDesc{};
	ValueType myImage{};
};

template <GraphicsBackend B>
class ImageView : public DeviceObject<B>
{
public:
	constexpr ImageView() noexcept = default;
	ImageView(ImageView&& other) noexcept;
	ImageView( // creates a view from image
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		const Image<B>& image,
		Flags<Vk> aspectFlags);
	~ImageView();

	ImageView& operator=(ImageView&& other) noexcept;
	operator auto() const noexcept { return myView; }

	void swap(ImageView& rhs) noexcept;
	friend void swap(ImageView& lhs, ImageView& rhs) noexcept { lhs.swap(rhs); }

private:
	ImageView( // uses provided image view
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		ImageViewHandle<B>&& view);

	ImageViewHandle<B> myView{};
};

#include "image.inl"
