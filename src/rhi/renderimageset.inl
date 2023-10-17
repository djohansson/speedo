namespace renderimageset
{

template <GraphicsApi G>
RenderTargetCreateDesc<G> createRenderTargetCreateDesc(
	const std::vector<std::shared_ptr<Image<G>>>& colorImages,
	const std::shared_ptr<Image<G>>& depthStencilImage)
{
	RenderTargetCreateDesc<G> outDesc{};

	assertf(colorImages.size(), "colorImages cannot be empty");

	auto firstColorImageExtent = colorImages.front()->getDesc().mipLevels[0].extent;

	outDesc.extent = {firstColorImageExtent.width, firstColorImageExtent.height};
	outDesc.colorImageFormats.reserve(colorImages.size());
	outDesc.colorImageLayouts.reserve(colorImages.size());
	outDesc.colorImages.reserve(colorImages.size());

	for (const auto& image : colorImages)
	{
		assertf(
			outDesc.extent.width == image->getDesc().mipLevels[0].extent.width,
			"all colorImages needs to have same width");
		assertf(
			outDesc.extent.height == image->getDesc().mipLevels[0].extent.height,
			"all colorImages needs to have same height");

		outDesc.colorImageFormats.emplace_back(image->getDesc().format);
		outDesc.colorImageLayouts.emplace_back(image->getImageLayout());
		outDesc.colorImages.emplace_back(*image);
	}

	if (depthStencilImage)
	{
		outDesc.depthStencilImageFormat = depthStencilImage->getDesc().format;
		outDesc.depthStencilImageLayout = depthStencilImage->getImageLayout();
		outDesc.depthStencilImage = *depthStencilImage;
	}

	outDesc.layerCount = 1;

	return outDesc;
}

} // namespace renderimageset

template <GraphicsApi G>
RenderImageSet<G>::RenderImageSet(
	const std::shared_ptr<Device<G>>& device,
	const std::vector<std::shared_ptr<Image<G>>>& colorImages,
	const std::shared_ptr<Image<G>>& depthStencilImage)
	: BaseType(
		  device,
		  renderimageset::createRenderTargetCreateDesc(colorImages, depthStencilImage))
	, myColorImages(colorImages)
	, myDepthStencilImage(depthStencilImage)
{}

template <GraphicsApi G>
RenderImageSet<G>::RenderImageSet(RenderImageSet&& other) noexcept
	: BaseType(std::forward<RenderImageSet>(other))
	, myColorImages(std::exchange(other.myColorImages, {}))
	, myDepthStencilImage(std::exchange(other.myDepthStencilImage, {}))
{}

template <GraphicsApi G>
RenderImageSet<G>::~RenderImageSet()
{}

template <GraphicsApi G>
RenderImageSet<G>& RenderImageSet<G>::operator=(RenderImageSet&& other) noexcept
{
	BaseType::operator=(std::forward<RenderImageSet>(other));
	myColorImages = std::exchange(other.myColorImages, {});
	myDepthStencilImage = std::exchange(other.myDepthStencilImage, {});
	return *this;
}

template <GraphicsApi G>
void RenderImageSet<G>::swap(RenderImageSet& rhs) noexcept
{
	BaseType::swap(rhs);
	std::swap(myColorImages, rhs.myColorImages);
	std::swap(myDepthStencilImage, rhs.myDepthStencilImage);
}

template <GraphicsApi G>
ImageLayout<G> RenderImageSet<G>::getColorImageLayout(uint32_t index) const
{
	return myColorImages[index]->getImageLayout();
}

template <GraphicsApi G>
ImageLayout<G> RenderImageSet<G>::getDepthStencilImageLayout() const
{
	return myDepthStencilImage->getImageLayout();
}

template <GraphicsApi G>
void RenderImageSet<G>::end(CommandBufferHandle<G> cmd)
{
	RenderTarget<G>::end(cmd);

	uint32_t imageIt = 0ul;
	for (; imageIt < myColorImages.size(); imageIt++)
		myColorImages[imageIt]->setImageLayout(this->getAttachmentDesc(imageIt).finalLayout);

	if (myDepthStencilImage)
		myDepthStencilImage->setImageLayout(this->getAttachmentDesc(imageIt).finalLayout);
}

template <GraphicsApi G>
void RenderImageSet<G>::transitionColor(
	CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index)
{
	myColorImages[index]->transition(cmd, layout);
}

template <GraphicsApi G>
void RenderImageSet<G>::transitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout)
{
	myDepthStencilImage->transition(cmd, layout);
}
