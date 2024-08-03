template <typename CreateDescType, GraphicsApi G>
class RenderTargetImpl : public RenderTarget<G>
{};

template <GraphicsApi G>
void RenderTarget<G>::AddSubpassDescription(SubpassDescription<G>&& description)
{
	mySubPassDescs.emplace_back(std::forward<SubpassDescription<G>>(description));
}

template <GraphicsApi G>
void RenderTarget<G>::AddSubpassDependency(SubpassDependency<G>&& dependency)
{
	mySubPassDependencies.emplace_back(std::forward<SubpassDependency<G>>(dependency));
}

template <GraphicsApi G>
void RenderTarget<G>::ResetSubpasses()
{
	mySubPassDescs.clear();
	mySubPassDependencies.clear();
}

template <GraphicsApi G>
AttachmentLoadOp<G> RenderTarget<G>::GetColorLoadOp(uint32_t index) const
{
	ASSERT(index < this->GetRenderTargetDesc().colorImages.size());

	return myAttachmentDescs[index].loadOp;
}

template <GraphicsApi G>
AttachmentStoreOp<G> RenderTarget<G>::GetColorStoreOp(uint32_t index) const
{
	ASSERT(index < this->GetRenderTargetDesc().colorImages.size());

	return myAttachmentDescs[index].storeOp;
}

template <GraphicsApi G>
AttachmentLoadOp<G> RenderTarget<G>::GetDepthStencilLoadOp() const
{
	ASSERT(myAttachmentDescs.size() > 0);

	return myAttachmentDescs[myAttachmentDescs.size() - 1].loadOp;
}

template <GraphicsApi G>
AttachmentStoreOp<G> RenderTarget<G>::GetDepthStencilStoreOp() const
{
	ASSERT(myAttachmentDescs.size() > 0);

	return myAttachmentDescs[myAttachmentDescs.size() - 1].storeOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetColorLoadOp(uint32_t index, AttachmentLoadOp<G> loadOp)
{
	ASSERT(index < this->GetRenderTargetDesc().colorImages.size());

	myAttachmentDescs[index].loadOp = loadOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetColorStoreOp(uint32_t index, AttachmentStoreOp<G> storeOp)
{
	ASSERT(index < this->GetRenderTargetDesc().colorImages.size());

	myAttachmentDescs[index].storeOp = storeOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetDepthStencilLoadOp(AttachmentLoadOp<G> loadOp)
{
	myAttachmentDescs[myAttachmentDescs.size() - 1].loadOp = loadOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetDepthStencilStoreOp(AttachmentStoreOp<G> storeOp)
{
	myAttachmentDescs[myAttachmentDescs.size() - 1].storeOp = storeOp;
}

#include "vulkan/rendertarget.inl"
