#pragma once

#include "device.h"
#include "types.h"

#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>
#include <tuple>

#include <xxh3.h>


template <GraphicsBackend B>
struct RenderTargetCreateDesc : DeviceResourceCreateDesc<B>
{
    Extent2d<B> imageExtent = {};
	std::vector<Format<B>> colorImageFormats;
    std::vector<ImageLayout<B>> colorImageLayouts;
    std::vector<ImageHandle<B>> colorImages;
    Format<B> depthStencilImageFormat = {};
    ImageLayout<B> depthStencilImageLayout = {};
    ImageHandle<B> depthStencilImage = 0; // optional
    uint32_t layerCount = 1;
    bool useDefaultInitialization = true; // create default render passes & framebuffer objects
};

template <GraphicsBackend B>
struct RenderTargetBeginInfo
{
    SubpassContents<B> contents;
};

template <GraphicsBackend B>
class RenderTarget : public DeviceResource<B>
{
public:

    virtual ~RenderTarget();
    
    virtual const RenderTargetCreateDesc<B>& getRenderTargetDesc() const = 0;

    const auto& getAttachments() const { return myAttachments; }
    const RenderPassHandle<B>& getRenderPass();
    const FramebufferHandle<B>& getFramebuffer();
    const auto& getSubpass() const { return myCurrentSubpass; }

    void begin(CommandBufferHandle<B> cmd, RenderTargetBeginInfo<B>&& beginInfo = {});
    void end(CommandBufferHandle<B> cmd);

    void clearSingle(
        CommandBufferHandle<B> cmd,
        const ClearAttachment<B>& clearAttachment) const;
    void clearAll(
        CommandBufferHandle<B> cmd,
        const ClearColorValue<B>& color = {},
        const ClearDepthStencilValue<B>& depthStencil = {}) const;
    
    void addSubpassDescription(SubpassDescription<B>&& description);
    void addSubpassDependency(SubpassDependency<B>&& dependency);
    void nextSubpass(CommandBufferHandle<B> cmd, SubpassContents<B> contents);
    void resetSubpasses();

    void setColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<B> op);
    void setColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<B> op);
    void setDepthStencilAttachmentLoadOp(AttachmentLoadOp<B> op);
    void setDepthStencilAttachmentStoreOp(AttachmentStoreOp<B> op);

protected:

    RenderTarget(RenderTarget<B>&& other) = default;
    RenderTarget(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const RenderTargetCreateDesc<B>& desc);

    virtual ImageLayout<B> getColorImageLayout(uint32_t index) const = 0;
    virtual ImageLayout<B> getDepthStencilImageLayout() const = 0;

private:

    using RenderPassFramebufferTuple = std::tuple<RenderPassHandle<B>, FramebufferHandle<B>>;
    using RenderPassFramebufferTupleMap = typename std::map<uint64_t, RenderPassFramebufferTuple>;

    uint64_t internalCalculateHashKey(const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc) const;    
    
    void internalInitializeAttachments(const RenderTargetCreateDesc<B>& desc);
    void internalInitializeDefaultRenderPasses(const RenderTargetCreateDesc<B>& desc);
    
    void internalUpdateAttachments(const RenderTargetCreateDesc<B>& desc);
    void internalUpdateRenderPasses(const RenderTargetCreateDesc<B>& desc);
    void internalUpdateMap(const RenderTargetCreateDesc<B>& desc);

    RenderPassFramebufferTuple
    internalCreateRenderPassAndFrameBuffer(uint64_t hashKey, const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc);

    std::vector<ImageViewHandle<B>> myAttachments;
    std::vector<AttachmentDescription<B>> myAttachmentsDescs;
    std::vector<AttachmentReference<B>> myAttachmentsReferences;
    std::vector<SubpassDescription<B>> mySubPassDescs;
    std::vector<SubpassDependency<B>> mySubPassDependencies;

    RenderPassFramebufferTupleMap myMap;

    std::optional<RenderTargetBeginInfo<B>> myCurrentPassInfo;
    std::optional<typename RenderPassFramebufferTupleMap::const_iterator> myCurrent;
    std::optional<uint32_t> myCurrentSubpass;

    std::shared_mutex myMutex;
    
    std::unique_ptr<XXH3_state_t, XXH_errorcode(*)(XXH3_state_t*)> myXXHState = { XXH3_createState(), XXH3_freeState };
};

template <typename CreateDescType, GraphicsBackend B>
class RenderTargetImpl : protected RenderTarget<B>
{ };

#include "rendertarget.inl"
#include "rendertarget-vulkan.inl"
