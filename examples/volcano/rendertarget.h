#pragma once

#include "device.h"
#include "types.h"

#include <optional>
#include <vector>
#include <tuple>

template <GraphicsBackend B>
struct RenderTargetCreateDesc
{
    Extent2d<B> extent = {};
	std::vector<Format<B>> colorImageFormats;
    std::vector<ImageLayout<B>> colorImageLayouts;
    std::vector<ImageHandle<B>> colorImages;
    Format<B> depthStencilImageFormat = {};
    ImageLayout<B> depthStencilImageLayout = {};
    ImageHandle<B> depthStencilImage = {}; // optional
    uint32_t layerCount = 1;
    bool useDefaultInitialization = true; // create default render passes & framebuffer objects
};

template <GraphicsBackend B>
struct IRenderTarget
{
    virtual const RenderTargetCreateDesc<B>& getRenderTargetDesc() const = 0;
    
    virtual ImageLayout<B> getColorImageLayout(uint32_t index) const = 0;
    virtual ImageLayout<B> getDepthStencilImageLayout() const = 0;

    virtual const std::optional<RenderPassBeginInfo<B>>& begin(CommandBufferHandle<B> cmd, SubpassContents<B> contents) = 0;
    virtual void end(CommandBufferHandle<B> cmd) = 0;

    virtual void blit(
        CommandBufferHandle<B> cmd,
        const IRenderTarget<B>& srcRenderTarget,
        const ImageSubresourceLayers<B>& srcSubresource,
        uint32_t srcIndex,
        const ImageSubresourceLayers<B>& dstSubresource,
        uint32_t dstIndex,
        Filter<B> filter = {}) = 0;

    virtual void clearSingleAttachment(
        CommandBufferHandle<B> cmd,
        const ClearAttachment<B>& clearAttachment) const = 0;
    virtual void clearAllAttachments(
        CommandBufferHandle<B> cmd,
        const ClearColorValue<B>& color = {},
        const ClearDepthStencilValue<B>& depthStencil = {}) const = 0;

    virtual void clearColor(CommandBufferHandle<B> cmd, const ClearColorValue<B>& color, uint32_t index) = 0;
    virtual void clearDepthStencil(CommandBufferHandle<B> cmd, const ClearDepthStencilValue<B>& depthStencil) = 0;

    virtual void transitionColor(CommandBufferHandle<B> cmd, ImageLayout<B> layout, uint32_t index) = 0;
    virtual void transitionDepthStencil(CommandBufferHandle<B> cmd, ImageLayout<B> layout) = 0;
};

template <GraphicsBackend B>
class RenderTarget : public IRenderTarget<B>, public DeviceObject<B>
{
    using ValueType = std::tuple<RenderPassHandle<B>, FramebufferHandle<B>>;
    using ValueMapType = UnorderedMap<uint64_t, ValueType>;

public:

    virtual ~RenderTarget();

    operator auto() { return std::get<0>(internalGetValues()); };

    virtual const std::optional<RenderPassBeginInfo<B>>& begin(CommandBufferHandle<B> cmd, SubpassContents<B> contents) final;
    virtual void end(CommandBufferHandle<B> cmd) override;

    virtual void blit(
        CommandBufferHandle<B> cmd,
        const IRenderTarget<B>& srcRenderTarget,
        const ImageSubresourceLayers<B>& srcSubresource,
        uint32_t srcIndex,
        const ImageSubresourceLayers<B>& dstSubresource,
        uint32_t dstIndex,
        Filter<B> filter = {}) final;

    virtual void clearSingleAttachment(
        CommandBufferHandle<B> cmd,
        const ClearAttachment<B>& clearAttachment) const final;
    virtual void clearAllAttachments(
        CommandBufferHandle<B> cmd,
        const ClearColorValue<B>& color = {},
        const ClearDepthStencilValue<B>& depthStencil = {}) const final;

    virtual void clearColor(CommandBufferHandle<B> cmd, const ClearColorValue<B>& color, uint32_t index) final;
    virtual void clearDepthStencil(CommandBufferHandle<B> cmd, const ClearDepthStencilValue<B>& depthStencil) final;

    auto getAttachment(uint32_t index) const noexcept { return myAttachments[index]; }
    const auto& getAttachmentDesc(uint32_t index) const noexcept { return myAttachmentDescs[index]; }
    const auto& getSubpass() const noexcept { return myCurrentSubpass; }
    
    void addSubpassDescription(SubpassDescription<B>&& description);
    void addSubpassDependency(SubpassDependency<B>&& dependency);
    void nextSubpass(CommandBufferHandle<B> cmd, SubpassContents<B> contents);
    void resetSubpasses();

    void setColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<B> op);
    void setColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<B> op);
    void setDepthStencilAttachmentLoadOp(AttachmentLoadOp<B> op);
    void setDepthStencilAttachmentStoreOp(AttachmentStoreOp<B> op);

protected:

    constexpr RenderTarget() noexcept = default;
    RenderTarget(RenderTarget<B>&& other) noexcept;
    RenderTarget(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const RenderTargetCreateDesc<B>& desc);

    RenderTarget& operator=(RenderTarget&& other) noexcept;

    void swap(RenderTarget& rhs) noexcept;

private:

    uint64_t internalCalculateHashKey(const RenderTargetCreateDesc<Vk>& desc) const;

    ValueType internalCreateRenderPassAndFrameBuffer(
        uint64_t hashKey,
        const RenderTargetCreateDesc<Vk>& desc);
    
    void internalInitializeAttachments(const RenderTargetCreateDesc<B>& desc);
    void internalInitializeDefaultRenderPass(const RenderTargetCreateDesc<B>& desc);
    
    void internalUpdateAttachments(const RenderTargetCreateDesc<B>& desc);
    void internalUpdateRenderPasses(const RenderTargetCreateDesc<B>& desc);

    const ValueType& internalUpdateMap(const RenderTargetCreateDesc<B>& desc);
    const ValueType& internalGetValues();

    std::vector<ImageViewHandle<B>> myAttachments;
    std::vector<AttachmentDescription<B>> myAttachmentDescs;
    std::vector<AttachmentReference<B>> myAttachmentsReferences;
    std::vector<SubpassDescription<B>> mySubPassDescs;
    std::vector<SubpassDependency<B>> mySubPassDependencies;

    ValueMapType myMap;

    std::optional<RenderPassBeginInfo<B>> myCurrentPass;
    std::optional<uint32_t> myCurrentSubpass;
};

#include "rendertarget.inl"
#include "rendertarget-vulkan.inl"
