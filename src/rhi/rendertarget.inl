template <typename CreateDescType, GraphicsApi G>
class RenderTargetImpl : public RenderTarget<G>
{};

template <GraphicsApi G>
void RenderTarget<G>::AddSubpassDescription(SubpassDescription<G>&& description)
{
	ASSERT(!myRenderInfo.has_value());
	
	mySubPassDescs.emplace_back(std::forward<SubpassDescription<G>>(description));
}

template <GraphicsApi G>
void RenderTarget<G>::AddSubpassDependency(SubpassDependency<G>&& dependency)
{
	ASSERT(!myRenderInfo.has_value());

	mySubPassDependencies.emplace_back(std::forward<SubpassDependency<G>>(dependency));
}

template <GraphicsApi G>
void RenderTarget<G>::ResetSubpasses()
{
	ASSERT(!myRenderInfo.has_value());

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
	ASSERT(!myRenderInfo.has_value());

	myAttachmentDescs[index].loadOp = loadOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetColorStoreOp(uint32_t index, AttachmentStoreOp<G> storeOp)
{
	ASSERT(index < this->GetRenderTargetDesc().colorImages.size());
	ASSERT(!myRenderInfo.has_value());

	myAttachmentDescs[index].storeOp = storeOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetDepthStencilLoadOp(AttachmentLoadOp<G> loadOp)
{
	ASSERT(!myRenderInfo.has_value());

	myAttachmentDescs[myAttachmentDescs.size() - 1].loadOp = loadOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetDepthStencilStoreOp(AttachmentStoreOp<G> storeOp)
{
	ASSERT(!myRenderInfo.has_value());
	
	myAttachmentDescs[myAttachmentDescs.size() - 1].storeOp = storeOp;
}

#include "vulkan/rendertarget.inl"
