template <typename CreateDescType, GraphicsBackend B>
class RenderTargetImpl : public RenderTarget<B>
{ };

template <GraphicsBackend B>
void RenderTarget<B>::addSubpassDescription(SubpassDescription<B>&& description)
{
    mySubPassDescs.emplace_back(std::forward<SubpassDescription<B>>(description));
    if (!myCurrentSubpass)
        myCurrentSubpass = std::make_optional<uint32_t>();
}

template <GraphicsBackend B>
void RenderTarget<B>::addSubpassDependency(SubpassDependency<B>&& dependency)
{
    mySubPassDependencies.emplace_back(std::forward<SubpassDependency<B>>(dependency));
}

template <GraphicsBackend B>
void RenderTarget<B>::resetSubpasses()
{
    mySubPassDescs.clear();
    mySubPassDependencies.clear();
    myCurrentSubpass.reset();
}

template <GraphicsBackend B>
void RenderTarget<B>::setColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<B> op)
{
    assert(index < this->getRenderTargetDesc().colorImages.size());
    
    myAttachmentDescs[index].loadOp = op;
}

template <GraphicsBackend B>
void RenderTarget<B>::setColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<B> op)
{
    assert(index < this->getRenderTargetDesc().colorImages.size());
    
    myAttachmentDescs[index].storeOp = op;
}

template <GraphicsBackend B>
void RenderTarget<B>::setDepthStencilAttachmentLoadOp(AttachmentLoadOp<B> op)
{
    myAttachmentDescs[myAttachmentDescs.size() - 1].loadOp = op;
}

template <GraphicsBackend B>
void RenderTarget<B>::setDepthStencilAttachmentStoreOp(AttachmentStoreOp<B> op)
{
    myAttachmentDescs[myAttachmentDescs.size() - 1].storeOp = op;
}
