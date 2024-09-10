namespace renderimageset
{

template <GraphicsApi G>
RenderTargetCreateDesc<G> createRenderTargetCreateDesc(
	const std::vector<std::shared_ptr<Image<G>>>& images)
{
	RenderTargetCreateDesc<G> outDesc{};

	ASSERTF(images.size(), "colorImages cannot be empty");

	auto firstImageExtent = images.front()->GetDesc().mipLevels[0].extent;

	outDesc.extent = {firstImageExtent.width, firstImageExtent.height};
	outDesc.imageFormats.reserve(images.size());
	outDesc.imageLayouts.reserve(images.size());
	outDesc.imageAspectFlags.reserve(images.size());
	outDesc.images.reserve(images.size());

	for (const auto& image : images)
	{
		ASSERTF(
			outDesc.extent.width == image->GetDesc().mipLevels[0].extent.width,
			"all images needs to have same width");
		ASSERTF(
			outDesc.extent.height == image->GetDesc().mipLevels[0].extent.height,
			"all images needs to have same height");

		outDesc.imageFormats.emplace_back(image->GetDesc().format);
		outDesc.imageLayouts.emplace_back(image->GetLayout());
		outDesc.imageAspectFlags.emplace_back(image->GetAspectFlags());
		outDesc.images.emplace_back(*image);
	}

	outDesc.layerCount = 1;

	return outDesc;
}

} // namespace renderimageset

template <GraphicsApi G>
RenderImageSet<G>::RenderImageSet(
	const std::shared_ptr<Device<G>>& device,
	const std::vector<std::shared_ptr<Image<G>>>& images)
	: BaseType(
		  device,
		  renderimageset::createRenderTargetCreateDesc(images))
	, myImages(images)
{}

template <GraphicsApi G>
RenderImageSet<G>::RenderImageSet(RenderImageSet&& other) noexcept
	: BaseType(std::forward<RenderImageSet>(other))
	, myImages(std::exchange(other.myImages, {}))
{}

template <GraphicsApi G>
RenderImageSet<G>::~RenderImageSet()
{}

template <GraphicsApi G>
RenderImageSet<G>& RenderImageSet<G>::operator=(RenderImageSet&& other) noexcept
{
	BaseType::operator=(std::forward<RenderImageSet>(other));
	myImages = std::exchange(other.myImages, {});
	return *this;
}

template <GraphicsApi G>
void RenderImageSet<G>::Swap(RenderImageSet& rhs) noexcept
{
	BaseType::Swap(rhs);
	std::swap(myImages, rhs.myImages);
}

template <GraphicsApi G>
ImageLayout<G> RenderImageSet<G>::GetLayout(uint32_t index) const
{
	return myImages[index]->GetLayout();
}

template <GraphicsApi G>
void RenderImageSet<G>::End(CommandBufferHandle<G> cmd)
{
	RenderTarget<G>::End(cmd);

	for (uint32_t imageIt = 0ul; imageIt < myImages.size(); imageIt++)
		myImages[imageIt]->InternalSetImageLayout(this->GetAttachmentDescs()[imageIt].finalLayout);
}

template <GraphicsApi G>
void RenderImageSet<G>::Transition(
	CommandBufferHandle<G> cmd, ImageLayout<G> layout, ImageAspectFlags<G> aspectFlags, uint32_t index)
{
	myImages[index]->Transition(cmd, layout, aspectFlags);
}
