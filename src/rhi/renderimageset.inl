namespace rhi
{

namespace renderimageset
{

template <GraphicsApi G, typename... Images>
RenderTargetCreateDesc<G> CreateRenderTargetCreateDesc(const Images&... images)
{
	RenderTargetCreateDesc<G> outDesc{};
	outDesc.uuid = uuids::NewUuid();

	auto imageCount = sizeof...(images);

	ENSUREF(imageCount, "colorImages cannot be empty");

	outDesc.imageFormats.reserve(imageCount);
	outDesc.imageLayouts.reserve(imageCount);
	outDesc.imageAspectFlags.reserve(imageCount);
	outDesc.images.reserve(imageCount);

	([&](size_t index)
	{
		const auto& image = images;
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
	} (0), ...);

	// todo: configure
	outDesc.layerCount = 1;
	outDesc.useDynamicRendering = true;

	return outDesc;
}

} // namespace renderimageset

template <GraphicsApi G>
template <typename... Images>
RenderImageSet<G>::RenderImageSet(Images&&... images)
	: SuperType(renderimageset::CreateRenderTargetCreateDesc<G>(images...)) // reads the images before they are moved below
	, myImages(std::make_shared<Image<G>[sizeof...(images)]>()) //NOLINT(modernize-avoid-c-arrays)
	, myImageCount(sizeof...(images))
{
	size_t index = 0;
	((myImages.get()[index++] = std::forward<Images>(images)), ...); //NOLINT(modernize-avoid-c-arrays)
}

template <GraphicsApi G>
RenderImageSet<G>::RenderImageSet(RenderImageSet&& other) noexcept
	: SuperType(std::forward<RenderImageSet>(other))
	, myImages(std::exchange(other.myImages, {}))
	, myImageCount(std::exchange(other.myImageCount, {}))
{}

template <GraphicsApi G>
RenderImageSet<G>::~RenderImageSet()
{}

template <GraphicsApi G>
RenderImageSet<G>& RenderImageSet<G>::operator=(RenderImageSet&& other) noexcept
{
	SuperType::operator=(std::forward<RenderImageSet>(other));
	myImages = std::exchange(other.myImages, {});
	myImageCount = std::exchange(other.myImageCount, {});
	return *this;
}

template <GraphicsApi G>
void RenderImageSet<G>::Swap(RenderImageSet& rhs) noexcept
{
	SuperType::Swap(rhs);
	std::swap(myImages, rhs.myImages);
	std::swap(myImageCount, rhs.myImageCount);
}

template <GraphicsApi G>
ImageLayout RenderImageSet<G>::GetLayout(uint32_t index) const
{
	auto& image = myImages.get()[index];
	return image.GetDesc().layout;
}

template <GraphicsApi G>
void RenderImageSet<G>::Transition(
	CommandBufferHandle<G> cmd, ImageLayout layout, ImageAspect aspectFlags, uint32_t index)
{
	auto& image = myImages.get()[index];
	image.Transition(cmd, layout, aspectFlags);
	
	this->InternalGetDesc().imageAspectFlags[index] = aspectFlags;
	this->InternalUpdateAttachments();
}

} // namespace rhi
