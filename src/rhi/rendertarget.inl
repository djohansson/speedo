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
void RenderTarget<G>::SetColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<G> op)
{
	ASSERT(index < this->GetRenderTargetDesc().colorImages.size());

	myAttachmentDescs[index].loadOp = op;
}

template <GraphicsApi G>
void RenderTarget<G>::SetColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<G> op)
{
	ASSERT(index < this->GetRenderTargetDesc().colorImages.size());

	myAttachmentDescs[index].storeOp = op;
}

template <GraphicsApi G>
void RenderTarget<G>::SetDepthStencilAttachmentLoadOp(AttachmentLoadOp<G> op)
{
	myAttachmentDescs[myAttachmentDescs.size() - 1].loadOp = op;
}

template <GraphicsApi G>
void RenderTarget<G>::SetDepthStencilAttachmentStoreOp(AttachmentStoreOp<G> op)
{
	myAttachmentDescs[myAttachmentDescs.size() - 1].storeOp = op;
}

#include "vulkan/rendertarget.inl"
