template <typename CreateDescType, GraphicsApi G>
class RenderTargetImpl : public RenderTarget<G>
{};

template <GraphicsApi G>
void RenderTarget<G>::addSubpassDescription(SubpassDescription<G>&& description)
{
	mySubPassDescs.emplace_back(std::forward<SubpassDescription<G>>(description));
}

template <GraphicsApi G>
void RenderTarget<G>::addSubpassDependency(SubpassDependency<G>&& dependency)
{
	mySubPassDependencies.emplace_back(std::forward<SubpassDependency<G>>(dependency));
}

template <GraphicsApi G>
void RenderTarget<G>::resetSubpasses()
{
	mySubPassDescs.clear();
	mySubPassDependencies.clear();
}

template <GraphicsApi G>
void RenderTarget<G>::setColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<G> op)
{
	assert(index < this->getRenderTargetDesc().colorImages.size());

	myAttachmentDescs[index].loadOp = op;
}

template <GraphicsApi G>
void RenderTarget<G>::setColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<G> op)
{
	assert(index < this->getRenderTargetDesc().colorImages.size());

	myAttachmentDescs[index].storeOp = op;
}

template <GraphicsApi G>
void RenderTarget<G>::setDepthStencilAttachmentLoadOp(AttachmentLoadOp<G> op)
{
	myAttachmentDescs[myAttachmentDescs.size() - 1].loadOp = op;
}

template <GraphicsApi G>
void RenderTarget<G>::setDepthStencilAttachmentStoreOp(AttachmentStoreOp<G> op)
{
	myAttachmentDescs[myAttachmentDescs.size() - 1].storeOp = op;
}

#include "vulkan/rendertarget.inl"
