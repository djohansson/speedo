#pragma once

#include "device.h"
#include "deviceresource.h"
#include "gfx-types.h"

#include <map>
#include <memory>
#include <vector>
#include <tuple>

template <GraphicsBackend B>
struct RenderTargetCreateDesc : DeviceResourceCreateDesc<B>
{
    Extent2d<B> imageExtent = {};
	std::vector<Format<B>> colorImageFormats;
    std::vector<ImageHandle<B>> colorImages;
    Format<B> depthStencilImageFormat = {};
    ImageHandle<B> depthStencilImage = 0; // optional
    uint32_t layerCount = 1;
};

template <GraphicsBackend B>
class RenderTarget : public DeviceResource<B>
{
public:

    virtual ~RenderTarget();
    
    virtual const RenderTargetCreateDesc<B>& getRenderTargetDesc() const = 0;

    const auto& getAttachments() const { return myAttachments; }
    const RenderPassHandle<B>& getRenderPass();
    const FramebufferHandle<B>& getFrameBuffer();

    void clear(CommandBufferHandle<B> cmd, const ClearAttachment<B>& clearAttachment) const;
    void clearAll(CommandBufferHandle<B> cmd, const ClearValue<B>& color = {}, const ClearValue<B>& depthStencil = {}) const;
    void nextSubpass(CommandBufferHandle<B> cmd, SubpassContents<B> contents) const;

    // void setColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<B> op);
    // void setColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<B> op);
    // void setDepthAttachmentLoadOp(AttachmentLoadOp<B> op);
    // void setDepthAttachmentStoreOp(AttachmentStoreOp<B> op);
    // void setStencilAttachmentLoadOp(AttachmentLoadOp<B> op);
    // void setDepthAttachmentStoreOp(AttachmentStoreOp<B> op);
    // void addSubpass(...)

protected:

    RenderTarget(RenderTarget<B>&& other);
    RenderTarget(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const RenderTargetCreateDesc<B>& desc);

private:

    void internalInitializeDefault(const RenderTargetCreateDesc<B>& desc);
    void internalCreateRenderPassAndFrameBuffer(const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc);

    std::vector<ImageViewHandle<B>> myAttachments;
    std::vector<AttachmentDescription<B>> myAttachmentsDescs;
    std::vector<AttachmentReference<B>> myAttachmentsReferences;
    std::vector<SubpassDescription<B>> mySubPassDescs;
    std::vector<SubpassDependency<B>> mySubPassDependencies;

    std::map<uint64_t, std::tuple<RenderPassHandle<B>, FramebufferHandle<B>>> myMap;
    uint64_t myKey = 0;
    bool myIsDirty = false;

    static constexpr std::string_view sc_colorImageViewStr = "_ColorImageView";
    static constexpr std::string_view sc_depthImageViewStr = "_DepthImageView";
    static constexpr std::string_view sc_framebufferStr = "_FrameBuffer";
    static constexpr std::string_view sc_renderPassStr = "_RenderPass";
};

template <typename CreateDescType, GraphicsBackend B>
class RenderTargetImpl : public RenderTarget<B>
{ };

#include "rendertarget-vulkan.inl"
