#include "rendertarget.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>

#include <xxhash.h>

namespace rendertarget_vulkan
{
    
thread_local static std::unique_ptr<XXH64_state_t, XXH_errorcode(*)(XXH64_state_t*)> t_xxhState(XXH64_createState(), XXH64_freeState);

}

template <>
void RenderTarget<GraphicsBackend::Vulkan>::internalInitializeDefault(const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc)
{
    char stringBuffer[128];
    
    myAttachments.clear();
    
    uint32_t attachmentIt = 0;
    for (; attachmentIt < desc.colorImages.size(); attachmentIt++)
    {
        myAttachments.emplace_back(createImageView2D(
            getDeviceContext()->getDevice(),
            desc.colorImages[attachmentIt],
            desc.colorImageFormats[attachmentIt],
            VK_IMAGE_ASPECT_COLOR_BIT));

        sprintf_s(
            stringBuffer,
            sizeof(stringBuffer),
            "%.*s%.*s%.*u",
            getName().size(),
            getName().c_str(),
            static_cast<int>(sc_colorImageViewStr.size()),
            sc_colorImageViewStr.data(),
            1,
            attachmentIt);

        addObject(
            VK_OBJECT_TYPE_IMAGE_VIEW,
            reinterpret_cast<uint64_t>(myAttachments.back()),
            stringBuffer);

        VkAttachmentDescription& colorAttachment = myAttachmentsDescs.emplace_back();
        colorAttachment.format = desc.colorImageFormats[attachmentIt];
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference& colorAttachmentRef = myAttachmentsReferences.emplace_back();
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    if (desc.depthStencilImage)
    {
        VkImageAspectFlags depthAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (hasStencilComponent(desc.depthStencilImageFormat))
            depthAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;

        myAttachments.emplace_back(createImageView2D(
            getDeviceContext()->getDevice(),
            desc.depthStencilImage,
            desc.depthStencilImageFormat,
            depthAspectFlags));

        sprintf_s(
            stringBuffer,
            sizeof(stringBuffer),
            "%.*s%.*s",
            getName().size(),
            getName().c_str(),
            static_cast<int>(sc_depthImageViewStr.size()),
            sc_depthImageViewStr.data());

        addObject(
            VK_OBJECT_TYPE_IMAGE_VIEW,
            reinterpret_cast<uint64_t>(myAttachments.back()),
            stringBuffer);

        VkAttachmentDescription& depthStencilAttachment = myAttachmentsDescs.emplace_back();
		depthStencilAttachment.format = desc.depthStencilImageFormat;
		depthStencilAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthStencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthStencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthStencilAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthStencilAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthStencilAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthStencilAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference& depthStencilAttachmentRef = myAttachmentsReferences.emplace_back();
        depthStencilAttachmentRef.attachment = attachmentIt;
        depthStencilAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        attachmentIt++;
    }

    assert(attachmentIt == myAttachmentsReferences.size());

    uint32_t subPassIt = 0;

    if (desc.depthStencilImage)
    {
        VkSubpassDescription& colorAndDepth = mySubPassDescs.emplace_back();
        colorAndDepth.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        colorAndDepth.colorAttachmentCount = myAttachmentsReferences.size() - 1;
        colorAndDepth.pColorAttachments = myAttachmentsReferences.data();
        colorAndDepth.pDepthStencilAttachment = &myAttachmentsReferences.back();

        VkSubpassDependency& dep0 = mySubPassDependencies.emplace_back();
        dep0.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep0.dstSubpass = subPassIt;
        dep0.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep0.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep0.srcAccessMask = 0;
        dep0.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep0.dependencyFlags = 0;

        VkSubpassDescription& color = mySubPassDescs.emplace_back();
        color.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        color.colorAttachmentCount = myAttachmentsReferences.size() - 1;
        color.pColorAttachments = myAttachmentsReferences.data();
        color.pDepthStencilAttachment = nullptr;

        VkSubpassDependency& dep1 = mySubPassDependencies.emplace_back();
        dep1.srcSubpass = subPassIt;
        dep1.dstSubpass = ++subPassIt;
        dep1.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep1.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep1.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        dep1.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep1.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    }
    else
    {
        VkSubpassDescription& color = mySubPassDescs.emplace_back();
        color.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        color.colorAttachmentCount = myAttachmentsReferences.size();
        color.pColorAttachments = myAttachmentsReferences.data();
        color.pDepthStencilAttachment = nullptr;

        VkSubpassDependency& dep0 = mySubPassDependencies.emplace_back();
        dep0.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep0.dstSubpass = subPassIt;
        dep0.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep0.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep0.srcAccessMask = 0;
        dep0.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep0.dependencyFlags = 0;
    }

    myIsDirty = true;
}

template <>
void RenderTarget<GraphicsBackend::Vulkan>::internalCreateRenderPassAndFrameBuffer(const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc)
{
    auto renderPass = createRenderPass(
        getDeviceContext()->getDevice(),
        myAttachmentsDescs,
        mySubPassDescs,
        mySubPassDependencies);
    
    auto frameBuffer = createFramebuffer(
        getDeviceContext()->getDevice(),
        renderPass,
        myAttachments.size(),
        myAttachments.data(),
        desc.imageExtent.width,
        desc.imageExtent.height,
        desc.layerCount);

    auto calculateHashKey = [this](const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc)
    {
        using namespace rendertarget_vulkan;

        assert(t_xxhState.get());

        constexpr XXH64_hash_t seed = 42;
        auto result = XXH64_reset(t_xxhState.get(), seed);
        assert(result != XXH_ERROR);

        result = XXH64_update(t_xxhState.get(), myAttachments.data(), myAttachments.size() * sizeof(myAttachments.front()));
        assert(result != XXH_ERROR);

        result = XXH64_update(t_xxhState.get(), myAttachmentsDescs.data(), myAttachmentsDescs.size() * sizeof(myAttachmentsDescs.front()));
        assert(result != XXH_ERROR);

        result = XXH64_update(t_xxhState.get(), myAttachmentsReferences.data(), myAttachmentsReferences.size() * sizeof(myAttachmentsReferences.front()));
        assert(result != XXH_ERROR);

        result = XXH64_update(t_xxhState.get(), mySubPassDescs.data(), mySubPassDescs.size() * sizeof(mySubPassDescs.front()));
        assert(result != XXH_ERROR);

        result = XXH64_update(t_xxhState.get(), mySubPassDependencies.data(), mySubPassDependencies.size() * sizeof(mySubPassDependencies.front()));
        assert(result != XXH_ERROR);

        result = XXH64_update(t_xxhState.get(), &desc.imageExtent, sizeof(desc.imageExtent));
        assert(result != XXH_ERROR);

        return XXH64_digest(t_xxhState.get());
    };

    myKey = calculateHashKey(desc);
    myMap[myKey] = std::make_tuple(renderPass, frameBuffer);

    char stringBuffer[128];

    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s%u",
        getName().size(),
        getName().c_str(),
        static_cast<int>(sc_renderPassStr.size()),
        sc_renderPassStr.data(),
        myKey);

    addObject(
        VK_OBJECT_TYPE_RENDER_PASS,
        reinterpret_cast<uint64_t>(renderPass),
        stringBuffer);

    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s%u",
        getName().size(),
        getName().c_str(),
        static_cast<int>(sc_framebufferStr.size()),
        sc_framebufferStr.data(),
        myKey);

    addObject(
        VK_OBJECT_TYPE_FRAMEBUFFER,
        reinterpret_cast<uint64_t>(frameBuffer),
        stringBuffer);

    myIsDirty = false;
}

template <>
void RenderTarget<GraphicsBackend::Vulkan>::clear(
    CommandBufferHandle<GraphicsBackend::Vulkan> cmd,
    const ClearAttachment<GraphicsBackend::Vulkan>& clearAttachment) const
{
    VkClearRect rect = { { { 0, 0 }, getRenderTargetDesc().imageExtent }, 0, getRenderTargetDesc().layerCount };
    vkCmdClearAttachments(cmd, 1, &clearAttachment, 1, &rect);
}

template <>
void RenderTarget<GraphicsBackend::Vulkan>::clearAll(
    CommandBufferHandle<GraphicsBackend::Vulkan> cmd,
    const ClearValue<GraphicsBackend::Vulkan>& color,
    const ClearValue<GraphicsBackend::Vulkan>& depthStencil) const
{
    uint32_t attachmentIt = 0;
    VkClearRect rect = { { { 0, 0 }, getRenderTargetDesc().imageExtent }, 0, getRenderTargetDesc().layerCount };
    std::vector<VkClearAttachment> clearAttachments(myAttachments.size(), { VK_IMAGE_ASPECT_COLOR_BIT, attachmentIt, color });

    for (auto& attachment : clearAttachments)
        attachment.colorAttachment = attachmentIt++;

    if (getRenderTargetDesc().depthStencilImage)
        clearAttachments.emplace_back(VkClearAttachment{ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, attachmentIt++, depthStencil });
    
    vkCmdClearAttachments(cmd, clearAttachments.size(), clearAttachments.data(), 1, &rect);
}

template <>
void RenderTarget<GraphicsBackend::Vulkan>::nextSubpass(
    CommandBufferHandle<GraphicsBackend::Vulkan> cmd,
    SubpassContents<GraphicsBackend::Vulkan> contents) const
{
    vkCmdNextSubpass(cmd, contents);
}

template <>
const RenderPassHandle<GraphicsBackend::Vulkan>& RenderTarget<GraphicsBackend::Vulkan>::getRenderPass()
{
    if (myIsDirty)
        internalCreateRenderPassAndFrameBuffer(getRenderTargetDesc());

    return std::get<0>(myMap.at(myKey));
}

template <>
const FramebufferHandle<GraphicsBackend::Vulkan>& RenderTarget<GraphicsBackend::Vulkan>::getFrameBuffer()
{
    if (myIsDirty)
        internalCreateRenderPassAndFrameBuffer(getRenderTargetDesc());

    return std::get<1>(myMap.at(myKey));
}

template <>
RenderTarget<GraphicsBackend::Vulkan>::RenderTarget(
    RenderTarget<GraphicsBackend::Vulkan>&& other)
: DeviceResource<GraphicsBackend::Vulkan>(std::move(other))
, myAttachments(std::move(other.myAttachments))
, myMap(std::move(other.myMap))
{
}

template <>
RenderTarget<GraphicsBackend::Vulkan>::RenderTarget(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc)
: DeviceResource<GraphicsBackend::Vulkan>(deviceContext, desc)
{
    ZoneScopedN("RenderTarget()");

    internalInitializeDefault(desc);
    internalCreateRenderPassAndFrameBuffer(desc);
}

template <>
RenderTarget<GraphicsBackend::Vulkan>::~RenderTarget()
{
    ZoneScopedN("~RenderTarget()");

    for (const auto& entry : myMap)
    {
        vkDestroyRenderPass(getDeviceContext()->getDevice(), std::get<0>(entry.second), nullptr);
        vkDestroyFramebuffer(getDeviceContext()->getDevice(), std::get<1>(entry.second), nullptr);
    }

    for (const auto& colorView : myAttachments)
        vkDestroyImageView(getDeviceContext()->getDevice(), colorView, nullptr);
}

template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    RenderTargetCreateDesc<GraphicsBackend::Vulkan>&& desc);

template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTargetImpl();
