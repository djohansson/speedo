namespace rhi
{

namespace renderimageset
{

template <GraphicsApi G>
RenderTargetCreateDesc<G> CreateRenderTargetCreateDesc(std::span<const std::shared_ptr<Image<G>>> images)
{
	RenderTargetCreateDesc<G> outDesc{};
	outDesc.uuid = uuids::NewUuid();

	ENSUREF(!images.empty(), "colorImages cannot be empty");

	outDesc.imageFormats.reserve(images.size());
	outDesc.imageLayouts.reserve(images.size());
	outDesc.imageAspectFlags.reserve(images.size());
	outDesc.images.reserve(images.size());

	for (size_t index = 0; index < images.size(); index++)
	{
		const auto& image = *images[index];
		auto extent = image.GetDesc().mipLevels[0].extent;

		// the render target lives on the same instance/device as its images
		outDesc.instance = image.GetDesc().instance;
		outDesc.device = image.GetDesc().device;
		if (index == 0)
			outDesc.name = std::format("{} RenderImageSet", image.GetName());

		ENSUREF(
			(outDesc.extent.width == 0 || outDesc.extent.width == extent.width),
			"all images needs to have same width");
		ENSUREF(
			(outDesc.extent.height == 0 || outDesc.extent.height == extent.height),
			"all images needs to have same height");
		
		outDesc.extent = extent;
		outDesc.imageFormats.emplace_back(image.GetDesc().format);
		outDesc.imageLayouts.emplace_back(image.GetDesc().layout);
		outDesc.imageAspectFlags.emplace_back(image.GetDesc().imageAspectFlags);
		outDesc.images.emplace_back(image);
	}

	// see RenderTarget::SetClearValue
	outDesc.clearValues.assign(images.size(), ClearValue{.color = {0.2F, 0.2F, 0.2F, 1.0F}, .depth = 1.0F, .stencil = 0});

	// todo: configure
	outDesc.layerCount = 1;
	outDesc.useDynamicRendering = true;

	return outDesc;
}

} // namespace renderimageset

template <GraphicsApi G>
RenderImageSet<G>::RenderImageSet(std::vector<std::shared_ptr<Image<G>>> images)
	: SuperType(renderimageset::CreateRenderTargetCreateDesc<G>(std::span<const std::shared_ptr<Image<G>>>(images)))
	, myImages(std::move(images))
{}

template <GraphicsApi G>
template <typename... Images>
	requires(std::same_as<std::remove_cvref_t<Images>, Image<G>> && ...)
RenderImageSet<G>::RenderImageSet(Images&&... images)
	: RenderImageSet(std::vector<std::shared_ptr<Image<G>>>{std::make_shared<Image<G>>(std::forward<Images>(images))...})
{}

template <GraphicsApi G>
RenderImageSet<G>::RenderImageSet(RenderImageSet&& other) noexcept
	: SuperType(std::forward<RenderImageSet>(other))
	, myImages(std::exchange(other.myImages, {}))
{}

template <GraphicsApi G>
RenderImageSet<G>::~RenderImageSet()
{}

template <GraphicsApi G>
RenderImageSet<G>& RenderImageSet<G>::operator=(RenderImageSet&& other) noexcept
{
	SuperType::operator=(std::forward<RenderImageSet>(other));
	myImages = std::exchange(other.myImages, {});
	return *this;
}

template <GraphicsApi G>
void RenderImageSet<G>::Swap(RenderImageSet& rhs) noexcept
{
	SuperType::Swap(rhs);
	std::swap(myImages, rhs.myImages);
}

template <GraphicsApi G>
ImageLayout RenderImageSet<G>::GetLayout(uint32_t index) const
{
	return myImages[index]->GetDesc().layout;
}

template <GraphicsApi G>
void RenderImageSet<G>::Transition(
	CommandBufferHandle<G> cmd, ImageLayout layout, ImageAspect aspectFlags, uint32_t index)
{
	myImages[index]->Transition(cmd, layout, aspectFlags);
	
	this->InternalGetDesc().imageAspectFlags[index] = aspectFlags;
	this->InternalUpdateAttachments();
}

} // namespace rhi
