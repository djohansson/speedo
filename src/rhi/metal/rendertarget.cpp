// the Metal backend's render targets (rhi/metal/rendertarget.inl): a render pass descriptor at each Begin (each
// attachment's texture, load and store actions and clear value), its render encoder open until End; clears are passes
// of their own (cleared, stored, ended at once); a copy is the blit encoder's. A clear of a rect inside a pass, and a
// scaled blit, need a draw (later)
#include <rhi/metal/backend.h>

#include <rhi/frame.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <utility>

namespace rhi
{

namespace rendertarget
{

static MTL::LoadAction ToMetal(LoadOp op) noexcept
{
	switch (op)
	{
	case LoadOp::kLoad: return MTL::LoadActionLoad;
	case LoadOp::kClear: return MTL::LoadActionClear;
	case LoadOp::kDontCare: return MTL::LoadActionDontCare;
	}
	return MTL::LoadActionDontCare;
}

static MTL::StoreAction ToMetal(StoreOp op) noexcept
{
	return op == StoreOp::kStore ? MTL::StoreActionStore : MTL::StoreActionDontCare;
}

// an attachment of a render pass descriptor: its texture, what the pass does with it, and its clear value
struct PassAttachment
{
	MTL::Texture* texture = nullptr;
	Format format{};
	LoadOp load = LoadOp::kDontCare;
	StoreOp store = StoreOp::kDontCare;
	LoadOp stencilLoad = LoadOp::kDontCare;
	StoreOp stencilStore = StoreOp::kDontCare;
	ClearValue clear{};
};

// a render pass over attachments, its encoder open on cmd: color attachments in their order, a depth stencil one's depth
// and stencil
static MTL::RenderCommandEncoder* BeginPass(metal::CommandBufferObject& cmd, std::span<const PassAttachment> attachments)
{
	auto* descriptor = MTL::RenderPassDescriptor::alloc()->init();
	NS::UInteger colorIt = 0;
	for (const auto& attachment : attachments)
	{
		if (HasColorComponent(attachment.format))
		{
			auto* color = descriptor->colorAttachments()->object(colorIt++);
			color->setTexture(attachment.texture);
			color->setLoadAction(ToMetal(attachment.load));
			color->setStoreAction(ToMetal(attachment.store));
			const auto& value = attachment.clear.color;
			color->setClearColor(MTL::ClearColor(value[0], value[1], value[2], value[3]));
			continue;
		}
		if (HasDepthComponent(attachment.format))
		{
			auto* depth = descriptor->depthAttachment();
			depth->setTexture(attachment.texture);
			depth->setLoadAction(ToMetal(attachment.load));
			depth->setStoreAction(ToMetal(attachment.store));
			depth->setClearDepth(attachment.clear.depth);
		}
		if (HasStencilComponent(attachment.format))
		{
			auto* stencil = descriptor->stencilAttachment();
			stencil->setTexture(attachment.texture);
			stencil->setLoadAction(ToMetal(attachment.stencilLoad));
			stencil->setStoreAction(ToMetal(attachment.stencilStore));
			stencil->setClearStencil(attachment.clear.stencil);
		}
	}
	auto* encoder = cmd.BeginRenderPass(descriptor);
	descriptor->release();
	return encoder;
}

} // namespace rendertarget

template <typename DerivedType>
RenderTarget<DerivedType, kMetal>::RenderTarget(typename ObjectTraits<DerivedType>::CreateDescType&& desc)
	: DeviceObject<DerivedType>(std::forward<typename ObjectTraits<DerivedType>::CreateDescType>(desc))
{
	const auto& formats = this->GetDesc().imageFormats;
	myOps.resize(formats.size());
	myAttachmentDescs.resize(formats.size());
	auto& rendering = myPipelineRenderingCreateInfo.emplace();
	for (auto format : formats)
	{
		if (HasColorComponent(format))
		{
			ENSUREF(rendering.colorAttachmentCount < kMaxColorAttachments, "a render target of more than {} color attachments", kMaxColorAttachments);
			rendering.colorAttachmentFormats[rendering.colorAttachmentCount++] = format;
			continue;
		}
		if (HasDepthComponent(format))
			rendering.depthAttachmentFormat = format;
		if (HasStencilComponent(format))
			rendering.stencilAttachmentFormat = format;
	}
	InternalUpdateAttachments();
}

template <typename DerivedType>
RenderTarget<DerivedType, kMetal>::RenderTarget(RenderTarget&& other) noexcept
{
	Swap(other);
}

template <typename DerivedType>
RenderTarget<DerivedType, kMetal>::~RenderTarget()
{
	InternalReleaseAttachments();
}

template <typename DerivedType>
RenderTarget<DerivedType, kMetal>& RenderTarget<DerivedType, kMetal>::operator=(RenderTarget&& other) noexcept
{
	Swap(other);
	return *this;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::Swap(RenderTarget& rhs) noexcept
{
	DeviceObject<DerivedType>::Swap(rhs);
	std::swap(myAttachments, rhs.myAttachments);
	std::swap(myAttachmentDescs, rhs.myAttachmentDescs);
	std::swap(myOps, rhs.myOps);
	std::swap(myPipelineRenderingCreateInfo, rhs.myPipelineRenderingCreateInfo);
	std::swap(myBeginInfo, rhs.myBeginInfo);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::InternalReleaseAttachments() noexcept
{
	for (auto* attachment : myAttachments)
	{
		attachment->texture->release();
		delete attachment;
	}
	myAttachments.clear();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::InternalUpdateAttachments()
{
	InternalReleaseAttachments();
	for (auto* texture : this->GetDesc().images)
		myAttachments.push_back(new metal::ImageViewObject{.texture = texture->retain()});
}

template <typename DerivedType>
const RenderTargetBeginInfo<kMetal>& RenderTarget<DerivedType, kMetal>::Begin(CommandBufferHandle<kMetal> cmd, SubpassContents contents)
{
	ZoneScopedN("RenderTarget::Begin");

	ENSUREF(contents == SubpassContents::kInline, "the Metal backend's passes record their commands inline (no secondary command buffers yet)");
	std::vector<rendertarget::PassAttachment> attachments;
	attachments.reserve(myAttachments.size());
	for (size_t attachmentIt = 0; attachmentIt < myAttachments.size(); attachmentIt++)
	{
		const auto& ops = myOps[attachmentIt];
		attachments.push_back({
			.texture = myAttachments[attachmentIt]->texture,
			.format = this->GetDesc().imageFormats[attachmentIt],
			.load = ops.load,
			.store = ops.store,
			.stencilLoad = ops.stencilLoad,
			.stencilStore = ops.stencilStore,
			.clear = this->GetDesc().clearValues[attachmentIt],
		});
	}
	rendertarget::BeginPass(*cmd, attachments);
	return myBeginInfo;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::End(CommandBufferHandle<kMetal> cmd)
{
	cmd->EndEncoders();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::ClearAll(CommandBufferHandle<kMetal> cmd, std::span<const ClearValue> values) const
{
	ZoneScopedN("RenderTarget::ClearAll");

	std::vector<rendertarget::PassAttachment> attachments;
	for (size_t attachmentIt = 0; attachmentIt < myAttachments.size(); attachmentIt++)
		attachments.push_back({
			.texture = myAttachments[attachmentIt]->texture,
			.format = this->GetDesc().imageFormats[attachmentIt],
			.load = LoadOp::kClear,
			.store = StoreOp::kStore,
			.stencilLoad = LoadOp::kClear,
			.stencilStore = StoreOp::kStore,
			.clear = attachmentIt < values.size() ? values[attachmentIt] : this->GetDesc().clearValues[attachmentIt],
		});
	rendertarget::BeginPass(*cmd, attachments);
	cmd->EndEncoders();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::Clear(CommandBufferHandle<kMetal> cmd, const ClearValue& value, uint32_t index)
{
	ZoneScopedN("RenderTarget::Clear");

	ENSURE(index < myAttachments.size());
	const std::array attachments{rendertarget::PassAttachment{
		.texture = myAttachments[index]->texture,
		.format = this->GetDesc().imageFormats[index],
		.load = LoadOp::kClear,
		.store = StoreOp::kStore,
		.stencilLoad = LoadOp::kClear,
		.stencilStore = StoreOp::kStore,
		.clear = value,
	}};
	rendertarget::BeginPass(*cmd, attachments);
	cmd->EndEncoders();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::ClearRect(
	CommandBufferHandle<kMetal> /*cmd*/, const ClearValue& /*value*/, uint32_t /*index*/, const Rect& /*rect*/) const
{
	ENSUREF(false, "the Metal backend clears no rect inside a pass yet (Metal clears a whole attachment as a pass loads it)");
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::Copy(
	CommandBufferHandle<kMetal> cmd,
	const IRenderTarget<kMetal>& srcRenderTarget,
	const ImageSubresourceLayers<kMetal>& /*srcSubresource*/,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kMetal>& /*dstSubresource*/,
	uint32_t dstIndex)
{
	ZoneScopedN("RenderTarget::Copy");

	// the whole of each (their first level and layer: an attachment's)
	cmd->BlitEncoder()->copyFromTexture(srcRenderTarget.GetImages()[srcIndex], this->GetDesc().images[dstIndex]);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::Blit(
	CommandBufferHandle<kMetal> cmd,
	const IRenderTarget<kMetal>& srcRenderTarget,
	const ImageSubresourceLayers<kMetal>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kMetal>& dstSubresource,
	uint32_t dstIndex,
	Filter /*filter*/)
{
	// of the same size, a copy; scaled, a draw (later)
	const auto srcExtent = srcRenderTarget.GetExtent();
	const auto dstExtent = GetExtent();
	ENSUREF(
		srcExtent.width == dstExtent.width && srcExtent.height == dstExtent.height,
		"the Metal backend blits between render targets of the same size alone yet ({}x{} to {}x{})",
		srcExtent.width,
		srcExtent.height,
		dstExtent.width,
		dstExtent.height);
	Copy(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp)
{
	myOps[index].load = loadOp;
	if (HasStencilComponent(this->GetDesc().imageFormats[index]))
		myOps[index].stencilLoad = stencilLoadOp;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp)
{
	myOps[index].store = storeOp;
	if (HasStencilComponent(this->GetDesc().imageFormats[index]))
		myOps[index].stencilStore = stencilStoreOp;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::SetClearValue(const ClearValue& value, uint32_t index)
{
	ENSURE(index < this->GetDesc().clearValues.size());
	this->InternalGetDesc().clearValues[index] = value;
}

// the backend's render targets (a swapchain's frames are made with it, step 6)
template class RenderTarget<RenderImageSet<kMetal>, kMetal>;
template class RenderTarget<Frame<kMetal>, kMetal>;

// a render image set's End: the pass leaves its images as Vulkan's render pass does (an attachment's layout: rhi's
// bookkeeping, Metal has none)
template <>
void RenderImageSet<kMetal>::End(CommandBufferHandle<kMetal> cmd)
{
	SuperType::End(cmd);
	for (uint32_t imageIt = 0; imageIt < GetImageCount(); imageIt++)
	{
		auto& image = *myImages[imageIt];
		image.InternalSetImageLayout(
			HasColorComponent(image.GetDesc().format) ? ImageLayout::kColorAttachment : ImageLayout::kDepthStencilAttachment);
	}
}

IMPLEMENT_OBJECT_GETINSTANCE(RenderImageSet<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(RenderImageSet<kMetal>);

} // namespace rhi
