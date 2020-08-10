#include "rendertarget.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>

template <>
void RenderTarget<Vk>::internalInitializeAttachments(const RenderTargetCreateDesc<Vk>& desc)
{
    char stringBuffer[128];

    static constexpr std::string_view colorImageViewStr = "_ColorImageView";
    static constexpr std::string_view depthImageViewStr = "_DepthImageView";
    
    myAttachments.clear();
    
    uint32_t attachmentIt = 0;
    for (; attachmentIt < desc.colorImages.size(); attachmentIt++)
    {
        myAttachments.emplace_back(createImageView2D(
            getDeviceContext()->getDevice(),
            0,
            desc.colorImages[attachmentIt],
            desc.colorImageFormats[attachmentIt],
            VK_IMAGE_ASPECT_COLOR_BIT));

        sprintf_s(
            stringBuffer,
            sizeof(stringBuffer),
            "%.*s%.*s%.*u",
            getName().size(),
            getName().c_str(),
            static_cast<int>(colorImageViewStr.size()),
            colorImageViewStr.data(),
            1,
            attachmentIt);

        getDeviceContext()->addOwnedObject(
            getId(),
            VK_OBJECT_TYPE_IMAGE_VIEW,
            reinterpret_cast<uint64_t>(myAttachments.back()),
            stringBuffer);

        auto& colorAttachment = myAttachmentDescs.emplace_back();
        colorAttachment.format = desc.colorImageFormats[attachmentIt];
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = desc.colorImageLayouts[attachmentIt];
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        auto& colorAttachmentRef = myAttachmentsReferences.emplace_back();
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
            0,
            desc.depthStencilImage,
            desc.depthStencilImageFormat,
            depthAspectFlags));

        sprintf_s(
            stringBuffer,
            sizeof(stringBuffer),
            "%.*s%.*s",
            getName().size(),
            getName().c_str(),
            static_cast<int>(depthImageViewStr.size()),
            depthImageViewStr.data());

        getDeviceContext()->addOwnedObject(
            getId(),
            VK_OBJECT_TYPE_IMAGE_VIEW,
            reinterpret_cast<uint64_t>(myAttachments.back()),
            stringBuffer);

        auto& depthStencilAttachment = myAttachmentDescs.emplace_back();
		depthStencilAttachment.format = desc.depthStencilImageFormat;
		depthStencilAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthStencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthStencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthStencilAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthStencilAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthStencilAttachment.initialLayout = desc.depthStencilImageLayout;
		depthStencilAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        auto& depthStencilAttachmentRef = myAttachmentsReferences.emplace_back();
        depthStencilAttachmentRef.attachment = attachmentIt;
        depthStencilAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        attachmentIt++;
    }

    assert(attachmentIt == myAttachmentsReferences.size());
}

template <>
void RenderTarget<Vk>::internalInitializeDefaultRenderPasses(const RenderTargetCreateDesc<Vk>& desc)
{
    uint32_t subPassIt = 0;

    if (desc.depthStencilImage)
    {
        // VkSubpassDescription depth = {};
        // depth.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        // depth.colorAttachmentCount = 0;
        // depth.pColorAttachments = nullptr;
        // depth.pDepthStencilAttachment = &myAttachmentsReferences.back();
        // addSubpassDescription(std::move(depth));

        // VkSubpassDependency dep0 = {};
        // dep0.srcSubpass = subPassIt;
        // dep0.dstSubpass = ++subPassIt;
        // dep0.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        // dep0.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        // dep0.srcAccessMask = 0;
        // dep0.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // dep0.dependencyFlags = 0;
        // addSubpassDependency(std::move(dep0));

        VkSubpassDescription colorAndDepth = {};
        colorAndDepth.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        colorAndDepth.colorAttachmentCount = myAttachmentsReferences.size() - 1;
        colorAndDepth.pColorAttachments = myAttachmentsReferences.data();
        colorAndDepth.pDepthStencilAttachment = &myAttachmentsReferences.back();
        addSubpassDescription(std::move(colorAndDepth));

        VkSubpassDependency dep1 = {};
        dep1.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep1.dstSubpass = subPassIt;
        dep1.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep1.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep1.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        dep1.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep1.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        addSubpassDependency(std::move(dep1));

        VkSubpassDescription color = {};
        color.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        color.colorAttachmentCount = myAttachmentsReferences.size() - 1;
        color.pColorAttachments = myAttachmentsReferences.data();
        color.pDepthStencilAttachment = nullptr;
        addSubpassDescription(std::move(color));

        VkSubpassDependency dep2 = {};
        dep2.srcSubpass = subPassIt;
        dep2.dstSubpass = ++subPassIt;
        dep2.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep2.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        dep2.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep2.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        addSubpassDependency(std::move(dep2));
    }
    else
    {
        VkSubpassDescription color = {};
        color.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        color.colorAttachmentCount = myAttachmentsReferences.size();
        color.pColorAttachments = myAttachmentsReferences.data();
        color.pDepthStencilAttachment = nullptr;
        addSubpassDescription(std::move(color));

        VkSubpassDependency dep0 = {};
        dep0.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep0.dstSubpass = subPassIt;
        dep0.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep0.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep0.srcAccessMask = 0;
        dep0.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep0.dependencyFlags = 0;
        addSubpassDependency(std::move(dep0));
    }
}

template <>
uint64_t RenderTarget<Vk>::internalCalculateHashKey(const RenderTargetCreateDesc<Vk>& desc) const
{
    constexpr XXH64_hash_t seed = 42;
    auto result = XXH3_64bits_reset_withSeed(myXXHState.get(), seed);
    assert(result != XXH_ERROR);

    result = XXH3_64bits_update(myXXHState.get(), myAttachments.data(), myAttachments.size() * sizeof(myAttachments.front()));
    assert(result != XXH_ERROR);

    result = XXH3_64bits_update(myXXHState.get(), myAttachmentDescs.data(), myAttachmentDescs.size() * sizeof(myAttachmentDescs.front()));
    assert(result != XXH_ERROR);

    result = XXH3_64bits_update(myXXHState.get(), myAttachmentsReferences.data(), myAttachmentsReferences.size() * sizeof(myAttachmentsReferences.front()));
    assert(result != XXH_ERROR);

    result = XXH3_64bits_update(myXXHState.get(), mySubPassDescs.data(), mySubPassDescs.size() * sizeof(mySubPassDescs.front()));
    assert(result != XXH_ERROR);

    result = XXH3_64bits_update(myXXHState.get(), mySubPassDependencies.data(), mySubPassDependencies.size() * sizeof(mySubPassDependencies.front()));
    assert(result != XXH_ERROR);

    result = XXH3_64bits_update(myXXHState.get(), &desc.imageExtent, sizeof(desc.imageExtent));
    assert(result != XXH_ERROR);

    return XXH3_64bits_digest(myXXHState.get());
}

template <>
RenderTarget<Vk>::RenderPassFramebufferTuple
RenderTarget<Vk>::internalCreateRenderPassAndFrameBuffer(uint64_t hashKey, const RenderTargetCreateDesc<Vk>& desc)
{
    char stringBuffer[128];
    
    static constexpr std::string_view renderPassStr = "_RenderPass";

    auto renderPass = createRenderPass(
        getDeviceContext()->getDevice(),
        myAttachmentDescs,
        mySubPassDescs,
        mySubPassDependencies);

    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s%u",
        getName().size(),
        getName().c_str(),
        static_cast<int>(renderPassStr.size()),
        renderPassStr.data(),
        hashKey);

    getDeviceContext()->addOwnedObject(
        getId(),
        VK_OBJECT_TYPE_RENDER_PASS,
        reinterpret_cast<uint64_t>(renderPass),
        stringBuffer);

    static constexpr std::string_view framebufferStr = "_FrameBuffer";
    
    auto frameBuffer = createFramebuffer(
        getDeviceContext()->getDevice(),
        renderPass,
        myAttachments.size(),
        myAttachments.data(),
        desc.imageExtent.width,
        desc.imageExtent.height,
        desc.layerCount);

    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s%u",
        getName().size(),
        getName().c_str(),
        static_cast<int>(framebufferStr.size()),
        framebufferStr.data(),
        hashKey);

    getDeviceContext()->addOwnedObject(
        getId(),
        VK_OBJECT_TYPE_FRAMEBUFFER,
        reinterpret_cast<uint64_t>(frameBuffer),
        stringBuffer);

    return std::make_tuple(renderPass, frameBuffer);
}

template <>
RenderTarget<Vk>::RenderPassFramebufferTupleMap::const_iterator RenderTarget<Vk>::internalUpdateMap(const RenderTargetCreateDesc<Vk>& desc)
{
    auto hashKey = internalCalculateHashKey(desc);
    auto emplaceResult = myMap.emplace(
        hashKey,
        std::make_tuple(
            RenderPassHandle<Vk>{},
            FramebufferHandle<Vk>{}));

    if (emplaceResult.second)
        emplaceResult.first->second = internalCreateRenderPassAndFrameBuffer(hashKey, desc);

    return emplaceResult.first;
}

template <>
void RenderTarget<Vk>::internalUpdateRenderPasses(const RenderTargetCreateDesc<Vk>& desc)
{

}

template <>
void RenderTarget<Vk>::internalUpdateAttachments(const RenderTargetCreateDesc<Vk>& desc)
{
    uint32_t attachmentIt = 0;
    for (; attachmentIt < desc.colorImages.size(); attachmentIt++)
    {
        auto& colorAttachment = myAttachmentDescs[attachmentIt];

        if (auto layout = getColorImageLayout(attachmentIt); layout != colorAttachment.initialLayout)
            colorAttachment.initialLayout = layout;
    }

    if (desc.depthStencilImage)
    {
        auto& depthStencilAttachment = myAttachmentDescs[attachmentIt];

        if (auto layout = getDepthStencilImageLayout(); layout != depthStencilAttachment.initialLayout)
            depthStencilAttachment.initialLayout = layout;
    }
}

template <>
void RenderTarget<Vk>::clearSingleAttachment(
    CommandBufferHandle<Vk> cmd,
    const ClearAttachment<Vk>& clearAttachment) const
{
    VkClearRect rect = { { { 0, 0 }, getRenderTargetDesc().imageExtent }, 0, getRenderTargetDesc().layerCount };
    vkCmdClearAttachments(cmd, 1, &clearAttachment, 1, &rect);
}

template <>
void RenderTarget<Vk>::clearAllAttachments(
    CommandBufferHandle<Vk> cmd,
    const ClearColorValue<Vk>& color,
    const ClearDepthStencilValue<Vk>& depthStencil) const
{
    uint32_t attachmentIt = 0;
    VkClearRect rect = { { { 0, 0 }, getRenderTargetDesc().imageExtent }, 0, getRenderTargetDesc().layerCount };
    std::vector<VkClearAttachment> clearAttachments(
        myAttachments.size(), {
            VK_IMAGE_ASPECT_COLOR_BIT,
            attachmentIt,
            { .color = color } });

    for (auto& attachment : clearAttachments)
        attachment.colorAttachment = attachmentIt++;

    if (getRenderTargetDesc().depthStencilImage)
        clearAttachments.emplace_back(
            VkClearAttachment{
                 VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                 attachmentIt++,
                 { .depthStencil = depthStencil } });
    
    vkCmdClearAttachments(cmd, clearAttachments.size(), clearAttachments.data(), 1, &rect);
}

template <>
void RenderTarget<Vk>::clearColor(
    CommandBufferHandle<Vk> cmd,
    const ClearColorValue<Vk>& color,
    uint32_t index)
{
    transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, index);
        
    VkImageSubresourceRange colorRange = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        VK_REMAINING_MIP_LEVELS,
        0,
        VK_REMAINING_ARRAY_LAYERS};

    vkCmdClearColorImage(
        cmd,
        getRenderTargetDesc().colorImages[index],
        getColorImageLayout(index),
        &color,
        1,
        &colorRange);
}

template <>
void RenderTarget<Vk>::clearDepthStencil(
    CommandBufferHandle<Vk> cmd,
    const ClearDepthStencilValue<Vk>& depthStencil)
{
    transitionDepthStencil(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageSubresourceRange depthStencilRange = {
        VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,
        0,
        VK_REMAINING_MIP_LEVELS,
        0,
        VK_REMAINING_ARRAY_LAYERS};

    vkCmdClearDepthStencilImage(
        cmd,
        getRenderTargetDesc().depthStencilImage,
        getDepthStencilImageLayout(),
        &depthStencil,
        1,
        &depthStencilRange);
}

template <>
void RenderTarget<Vk>::nextSubpass(
    CommandBufferHandle<Vk> cmd,
    SubpassContents<Vk> contents)
{
    vkCmdNextSubpass(cmd, contents);

    (*myCurrentSubpass)++;
}

template <>
RenderPassHandle<Vk> RenderTarget<Vk>::renderPass()
{
    internalUpdateAttachments(getRenderTargetDesc());
    internalUpdateRenderPasses(getRenderTargetDesc());

    return std::get<0>(internalUpdateMap(getRenderTargetDesc())->second);
}

template <>
FramebufferHandle<Vk> RenderTarget<Vk>::framebuffer()
{
    internalUpdateAttachments(getRenderTargetDesc());
    internalUpdateRenderPasses(getRenderTargetDesc());
    
    return std::get<1>(internalUpdateMap(getRenderTargetDesc())->second);
}

template <>
const std::optional<RenderPassBeginInfo<Vk>>& RenderTarget<Vk>::begin(
    CommandBufferHandle<Vk> cmd,
    SubpassContents<Vk> contents)
{
    ZoneScopedN("RenderTarget::begin()");

    assert(myCurrentPass == std::nullopt);

    myCurrentPass = std::make_optional(RenderPassBeginInfo<Vk>{
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        nullptr, 
        renderPass(),
        framebuffer(),
        {{0, 0}, {getRenderTargetDesc().imageExtent.width, getRenderTargetDesc().imageExtent.height}}});

    vkCmdBeginRenderPass(cmd, &myCurrentPass.value(), contents);

    return myCurrentPass;
}

template <>
void RenderTarget<Vk>::end(CommandBufferHandle<Vk> cmd)
{
    ZoneScopedN("RenderTarget::end()");

    assert(myCurrentPass != std::nullopt);

    vkCmdEndRenderPass(cmd);

    myCurrentPass = std::nullopt;
}

template <>
RenderTarget<Vk>::RenderTarget(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const RenderTargetCreateDesc<Vk>& desc)
: DeviceResource<Vk>(deviceContext, desc)
{
    ZoneScopedN("RenderTarget()");

    internalInitializeAttachments(desc);
    
    if (desc.useDefaultInitialization)
    {
        internalInitializeDefaultRenderPasses(desc);
        internalUpdateMap(desc);
    }
}

template <>
RenderTarget<Vk>::~RenderTarget()
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

template RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::RenderTargetImpl(
    RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>&& other);

template <>
RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    RenderTargetCreateDesc<Vk>&& desc)
: RenderTarget<Vk>(deviceContext, desc)
, myDesc(std::move(desc))
{
}

template RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::~RenderTargetImpl();

template RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>& RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::operator=(
    RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>&& other);
