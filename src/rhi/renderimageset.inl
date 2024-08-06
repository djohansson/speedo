namespace renderimageset
{

template <GraphicsApi G>
RenderTargetCreateDesc<G> createRenderTargetCreateDesc(
	const std::vector<std::shared_ptr<Image<G>>>& colorImages,
	const std::shared_ptr<Image<G>>& depthStencilImage)
{
	RenderTargetCreateDesc<G> outDesc{};
	outDesc.useDynamicRendering = false; // todo: make configurable

	ASSERTF(colorImages.size(), "colorImages cannot be empty");

	auto firstColorImageExtent = colorImages.front()->GetDesc().mipLevels[0].extent;

	outDesc.extent = {firstColorImageExtent.width, firstColorImageExtent.height};
	outDesc.colorImageFormats.reserve(colorImages.size());
	outDesc.colorImageLayouts.reserve(colorImages.size());
	outDesc.colorImages.reserve(colorImages.size());

	for (const auto& image : colorImages)
	{
		ASSERTF(
			outDesc.extent.width == image->GetDesc().mipLevels[0].extent.width,
			"all colorImages needs to have same width");
		ASSERTF(
			outDesc.extent.height == image->GetDesc().mipLevels[0].extent.height,
			"all colorImages needs to have same height");

		outDesc.colorImageFormats.emplace_back(image->GetDesc().format);
		outDesc.colorImageLayouts.emplace_back(image->GetImageLayout());
		outDesc.colorImages.emplace_back(*image);
	}

	if (depthStencilImage)
	{
		outDesc.depthStencilImageFormat = depthStencilImage->GetDesc().format;
		outDesc.depthStencilImageLayout = depthStencilImage->GetImageLayout();
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
void RenderImageSet<G>::Swap(RenderImageSet& rhs) noexcept
{
	BaseType::Swap(rhs);
	std::swap(myColorImages, rhs.myColorImages);
	std::swap(myDepthStencilImage, rhs.myDepthStencilImage);
}

template <GraphicsApi G>
ImageLayout<G> RenderImageSet<G>::GetColorLayout(uint32_t index) const
{
	return myColorImages[index]->GetImageLayout();
}

template <GraphicsApi G>
ImageLayout<G> RenderImageSet<G>::GetDepthStencilLayout() const
{
	return myDepthStencilImage->GetImageLayout();
}

template <GraphicsApi G>
void RenderImageSet<G>::End(CommandBufferHandle<G> cmd)
{
	RenderTarget<G>::End(cmd);

	uint32_t imageIt = 0ul;
	for (; imageIt < myColorImages.size(); imageIt++)
		myColorImages[imageIt]->InternalSetImageLayout(this->GetAttachmentDesc(imageIt).finalLayout);

	if (myDepthStencilImage)
		myDepthStencilImage->InternalSetImageLayout(this->GetAttachmentDesc(imageIt).finalLayout);
}

template <GraphicsApi G>
void RenderImageSet<G>::TransitionColor(
	CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index)
{
	myColorImages[index]->Transition(cmd, layout);
}

template <GraphicsApi G>
void RenderImageSet<G>::TransitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout)
{
	myDepthStencilImage->Transition(cmd, layout);
}
