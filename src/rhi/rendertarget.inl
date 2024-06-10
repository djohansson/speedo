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
void RenderTarget<G>::SetColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<G> loadOp)
{
	ASSERT(index < this->GetRenderTargetDesc().colorImages.size());

	myAttachmentDescs[index].loadOp = loadOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<G> storeOp)
{
	ASSERT(index < this->GetRenderTargetDesc().colorImages.size());

	myAttachmentDescs[index].storeOp = storeOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetDepthStencilAttachmentLoadOp(AttachmentLoadOp<G> loadOp)
{
	myAttachmentDescs[myAttachmentDescs.size() - 1].loadOp = loadOp;
}

template <GraphicsApi G>
void RenderTarget<G>::SetDepthStencilAttachmentStoreOp(AttachmentStoreOp<G> storeOp)
{
	myAttachmentDescs[myAttachmentDescs.size() - 1].storeOp = storeOp;
}

#include "vulkan/rendertarget.inl"
