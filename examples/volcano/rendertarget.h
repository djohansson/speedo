#pragma once

#include "device.h"
#include "deviceresource.h"
#include "gfx-types.h"

#include <map>
#include <memory>
#include <optional>
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
    bool useDefaultInitialization = true; // create default render passes & framebuffer objects
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
    const auto& getSubpass() const { return myCurrentSubpass; }

    void clear(CommandBufferHandle<B> cmd, const ClearAttachment<B>& clearAttachment) const;
    void clearAll(CommandBufferHandle<B> cmd, const ClearValue<B>& color = {}, const ClearValue<B>& depthStencil = {}) const;
    
    void addSubpassDescription(SubpassDescription<B>&& description);
    void addSubpassDependency(SubpassDependency<B>&& dependency);
    void nextSubpass(CommandBufferHandle<B> cmd, SubpassContents<B> contents);
    void resetSubpasses();

    void setColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<B> op);
    void setColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<B> op);
    void setDepthStencilAttachmentLoadOp(AttachmentLoadOp<B> op);
    void setDepthStencilAttachmentStoreOp(AttachmentStoreOp<B> op);

protected:

    RenderTarget(RenderTarget<B>&& other);
    RenderTarget(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const RenderTargetCreateDesc<B>& desc);

private:

    using RenderPassFramebufferTuple = std::tuple<RenderPassHandle<B>, FramebufferHandle<B>>;
    using RenderPassFramebufferTupleMap = typename std::map<uint64_t, RenderPassFramebufferTuple>;

    uint64_t internalCalculateHashKey(const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc) const;    
    void internalInitializeAttachments(const RenderTargetCreateDesc<B>& desc);
    void internalInitializeDefaultRenderPasses(const RenderTargetCreateDesc<B>& desc);
    void internalUpdateMap(const RenderTargetCreateDesc<B>& desc);

    RenderPassFramebufferTuple
    internalCreateRenderPassAndFrameBuffer(uint64_t hashKey, const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc);

    std::vector<ImageViewHandle<B>> myAttachments;
    std::vector<AttachmentDescription<B>> myAttachmentsDescs;
    std::vector<AttachmentReference<B>> myAttachmentsReferences;
    std::vector<SubpassDescription<B>> mySubPassDescs;
    std::vector<SubpassDependency<B>> mySubPassDependencies;

    RenderPassFramebufferTupleMap myMap;
    std::optional<typename RenderPassFramebufferTupleMap::const_iterator> myCurrent;
    std::optional<uint32_t> myCurrentSubpass;

    static constexpr std::string_view sc_colorImageViewStr = "_ColorImageView";
    static constexpr std::string_view sc_depthImageViewStr = "_DepthImageView";
    static constexpr std::string_view sc_framebufferStr = "_FrameBuffer";
    static constexpr std::string_view sc_renderPassStr = "_RenderPass";
};

template <typename CreateDescType, GraphicsBackend B>
class RenderTargetImpl : public RenderTarget<B>
{ };

#include "rendertarget.inl"
#include "rendertarget-vulkan.inl"
