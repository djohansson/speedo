#include "window.h"
#include "command.h"
#include "rendertarget.h"
#include "gfx.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>

#if defined(__WINDOWS__)
#include <execution>
#endif
#include <filesystem>
#include <future>
#include <numeric>
#include <string>
#include <string_view>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <examples/imgui_impl_vulkan.h>


template <>
void WindowContext<GraphicsBackend::Vulkan>::renderIMGUI()
{
    ZoneScopedN("renderIMGUI");
    
    using namespace ImGui;

    ImGui_ImplVulkan_NewFrame();
    NewFrame();

    for (auto callback : myIMGUIDrawCallbacks)
        callback();

    Render();
}

template <>
void WindowContext<GraphicsBackend::Vulkan>::createFrameObjects(Extent2d<GraphicsBackend::Vulkan> frameBufferExtent)
{
    ZoneScopedN("createFrameObjects");
    
    mySwapchain = std::make_unique<SwapchainContext<GraphicsBackend::Vulkan>>(
        myDevice,
        SwapchainCreateDesc<GraphicsBackend::Vulkan>{
            {"mySwapchain"},
            frameBufferExtent,
            mySwapchain ? mySwapchain->getSwapchain() : nullptr});

    myViewBuffer = std::make_unique<Buffer<GraphicsBackend::Vulkan>>(
        myDevice,
        BufferCreateDesc<GraphicsBackend::Vulkan>{
            {"myViewBuffer"},
            myDevice->getDesc().swapchainConfiguration->imageCount * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height) * sizeof(WindowContext::ViewBufferData),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT});

    myViews.resize(myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height);
    for (auto& view : myViews)
    {
        if (!view.desc().viewport.width)
            view.desc().viewport.width =  frameBufferExtent.width / myDesc.splitScreenGrid.width;

        if (!view.desc().viewport.height)
            view.desc().viewport.height = frameBufferExtent.height / myDesc.splitScreenGrid.height;

        view.updateAll();
    }

    uint32_t imageCount;
    VK_CHECK(vkGetSwapchainImagesKHR(myDevice->getDevice(), mySwapchain->getSwapchain(), &imageCount, nullptr));
    
    std::vector<ImageHandle<GraphicsBackend::Vulkan>> colorImages(imageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(myDevice->getDevice(), mySwapchain->getSwapchain(), &imageCount, colorImages.data()));

    uint32_t frameCount = myDevice->getDesc().swapchainConfiguration->imageCount;
    
    myFrames.clear();
    myFrames.reserve(frameCount);
    
    for (uint32_t frameIt = 0; frameIt < frameCount; frameIt++)
        myFrames.emplace_back(std::make_shared<Frame<GraphicsBackend::Vulkan>>(
            myDevice,
            FrameCreateDesc<GraphicsBackend::Vulkan>{
                {{"Frame"},
                { frameBufferExtent.width, frameBufferExtent.height },
                make_vector(myDevice->getDesc().swapchainConfiguration->surfaceFormat.format),
                make_vector(VK_IMAGE_LAYOUT_UNDEFINED),
                make_vector(colorImages[frameIt])},
                frameIt}));

    myFrameTimestamps.clear();
    myFrameTimestamps.resize(frameCount);
}

template <>
void WindowContext<GraphicsBackend::Vulkan>::updateViewBuffer(uint32_t frameIndex) const
{
    ZoneScopedN("updateViewBuffer");

    assert(frameIndex < myFrames.size());

    void* data;
    VK_CHECK(vmaMapMemory(myDevice->getAllocator(), myViewBuffer->getBufferMemory(), &data));

    for (uint32_t n = 0; n < (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height); n++)
    {
        ViewBufferData& ubo = reinterpret_cast<ViewBufferData*>(data)[frameIndex * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height) + n];

        ubo.viewProj = myViews[n].getProjectionMatrix() * glm::mat4(myViews[n].getViewMatrix());
    }

    vmaFlushAllocation(
        myDevice->getAllocator(),
        myViewBuffer->getBufferMemory(),
        sizeof(ViewBufferData) * frameIndex * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height),
        sizeof(ViewBufferData) * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height));

    vmaUnmapMemory(myDevice->getAllocator(), myViewBuffer->getBufferMemory());
}

template <>
std::tuple<bool, uint32_t, uint64_t> WindowContext<GraphicsBackend::Vulkan>::flipFrame(uint32_t lastFrameIndex) const
{
    ZoneScoped;

    static constexpr std::string_view flipFrameStr = "flipFrame";

    auto& lastFrame = myFrames[lastFrameIndex];

    uint32_t frameIndex;
    auto flipResult = checkFlipOrPresentResult(
        vkAcquireNextImageKHR(
            myDevice->getDevice(),
            mySwapchain->getSwapchain(),
            UINT64_MAX,
            lastFrame->getNewImageAcquiredSemaphore(),
            0,
            &frameIndex));

    if (flipResult != VK_SUCCESS)
    {
        //assert(lastFrameIndex == frameIndex); // just check that vkAcquireNextImageKHR is not messing up things

        static constexpr std::string_view errorStr = " - ERROR: vkAcquireNextImageKHR failed";

        char failedStr[flipFrameStr.size() + errorStr.size() + 1];
        sprintf_s(failedStr, sizeof(failedStr), "%.*s%.*s",
            static_cast<int>(flipFrameStr.size()), flipFrameStr.data(),
            static_cast<int>(errorStr.size()), errorStr.data());

        ZoneName(failedStr, sizeof_array(failedStr));

        // todo: print error code
        //ZoneText(failedStr, sizeof_array(failedStr));

        return std::make_tuple(false, lastFrameIndex, lastFrame->getLastSubmitTimelineValue());
    }

    char flipFrameWithNumberStr[flipFrameStr.size()+2];
    sprintf_s(flipFrameWithNumberStr, sizeof(flipFrameWithNumberStr), "%.*s%u",
        static_cast<int>(flipFrameStr.size()), flipFrameStr.data(), frameIndex);

    ZoneName(flipFrameWithNumberStr, sizeof_array(flipFrameWithNumberStr));

    auto& frame = myFrames[frameIndex];
    
    return std::make_tuple(true, frameIndex, frame->getLastSubmitTimelineValue());
}

template <>
uint64_t WindowContext<GraphicsBackend::Vulkan>::submitFrame(
    uint32_t frameIndex,
    uint32_t lastFrameIndex,
    const PipelineConfiguration<GraphicsBackend::Vulkan>& config,
    uint64_t waitTimelineValue)
{
    ZoneScopedN("submitFrame");

    myFrameTimestamps[frameIndex] = std::chrono::high_resolution_clock::now();

    auto& frame = myFrames[frameIndex];
    auto& lastFrame = myFrames[lastFrameIndex];
    auto& primaryCommandContext = commandContexts()[frameIndex][0];

    // {
    //     ZoneScopedN("tracyVkCollect");

    //     for (uint32_t contextIt = 0; contextIt < 1/*frame->commandContexts().size()*/; contextIt++)
    //     {
    //         auto& commandContext = commandContexts()[frameIndex][contextIt];
    //         auto cmd = commandContext->beginScope();

    //         TracyVkCollect(
    //             commandContext->userData<command_vulkan::UserData>().tracyContext,
    //             cmd);
    //     }
    // }

    std::future<void> updateViewBufferFuture(std::async(std::launch::async, [this, frameIndex]
    {
        updateViewBuffer(frameIndex);
    }));

    std::future<void> renderIMGUIFuture(std::async(
        std::launch::async,
        [this, &config, &frame](uint32_t commandContextIndex)
    {
        renderIMGUI();

        // Record Imgui Draw Data and draw funcs into primary command buffer
        {        
            ZoneScopedN("drawIMGUI");

            CommandBufferInheritanceInfo<GraphicsBackend::Vulkan> inherit = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
            inherit.renderPass = config.resources->renderTarget->getRenderPass();
            inherit.framebuffer = config.resources->renderTarget->getFramebuffer();

            CommandContextBeginInfo<GraphicsBackend::Vulkan> secBeginInfo = {};
            secBeginInfo.flags =
                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
            secBeginInfo.pInheritanceInfo = &inherit;
            secBeginInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;

            auto& commandContext = commandContexts()[frame->getDesc().index][commandContextIndex];
            auto cmd = commandContext->commands(std::move(secBeginInfo));

            // TracyVkZone(
            //     commandContext->userData<command_vulkan::UserData>().tracyContext,
            //     cmd, "drawIMGUI");

            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        }
    }, 0));

    // setup draw parameters
    uint32_t drawCount = myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height;
    uint32_t drawCommandContextCount = static_cast<uint32_t>(commandContexts()[frameIndex].size()) - 1;
    uint32_t drawThreadCount = std::min<uint32_t>(drawCount, drawCommandContextCount);

    std::atomic_uint32_t drawAtomic = 0;

    // draw views using secondary command buffers
    // todo: generalize this to other types of draws
    {
        ZoneScopedN("drawViews");

        std::array<uint32_t, 128> seq;
        std::iota(seq.begin(), seq.begin() + drawCommandContextCount, 1);
        std::for_each_n(
#if defined(__WINDOWS__)
            std::execution::par,
#endif
            seq.begin(), drawThreadCount,
            [this, &config, &frame, &drawAtomic, &drawCount](uint32_t threadIt)
            {
                ZoneScoped;

                auto drawIt = drawAtomic++;
                if (drawIt >= drawCount)
                    return;

                static constexpr std::string_view drawPartitionStr = "drawPartition";
                char drawPartitionWithNumberStr[drawPartitionStr.size()+3];
                sprintf_s(drawPartitionWithNumberStr, sizeof(drawPartitionWithNumberStr), "%.*s%u",
                    static_cast<int>(drawPartitionStr.size()), drawPartitionStr.data(), threadIt);

                ZoneName(drawPartitionWithNumberStr, sizeof_array(drawPartitionWithNumberStr));
                
                CommandBufferInheritanceInfo<GraphicsBackend::Vulkan> inherit = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
                inherit.renderPass = config.resources->renderTarget->getRenderPass();
                inherit.framebuffer = config.resources->renderTarget->getFramebuffer();

                CommandContextBeginInfo<GraphicsBackend::Vulkan> secBeginInfo = {};
                secBeginInfo.flags =
                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                    VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
                secBeginInfo.pInheritanceInfo = &inherit;
                secBeginInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                    
                auto& commandContext = commandContexts()[frame->getDesc().index][threadIt];
                auto cmd = commandContext->commands(std::move(secBeginInfo));

                // TracyVkZone(
                //     commandContext->userData<command_vulkan::UserData>().tracyContext,
                //     cmd, drawPartitionStr.data());
            
                // bind pipeline and inputs
                {
                    
                    // bind pipeline and vertex/index buffers
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, config.graphicsPipeline);

                    VkBuffer vertexBuffers[] = {config.resources->model->getBuffer().getBufferHandle()};
                    VkDeviceSize vertexOffsets[] = {config.resources->model->getVertexOffset()};

                    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
                    vkCmdBindIndexBuffer(cmd, config.resources->model->getBuffer().getBufferHandle(),
                        config.resources->model->getIndexOffset(), VK_INDEX_TYPE_UINT32);
                }

                uint32_t dx = frame->getDesc().imageExtent.width / myDesc.splitScreenGrid.width;
                uint32_t dy = frame->getDesc().imageExtent.height / myDesc.splitScreenGrid.height;

                while (drawIt < drawCount)
                {
                    auto drawView = [this, &drawCount, &frame, &config, &cmd, &dx, &dy](uint32_t viewIt)
                    {
                        uint32_t i = viewIt % myDesc.splitScreenGrid.width;
                        uint32_t j = viewIt / myDesc.splitScreenGrid.width;

                        auto setViewportAndScissor = [](VkCommandBuffer cmd, int32_t x, int32_t y,
                                                        int32_t width, int32_t height)
                        {
                            VkViewport viewport = {};
                            viewport.x = static_cast<float>(x);
                            viewport.y = static_cast<float>(y);
                            viewport.width = static_cast<float>(width);
                            viewport.height = static_cast<float>(height);
                            viewport.minDepth = 0.0f;
                            viewport.maxDepth = 1.0f;

                            VkRect2D scissor = {};
                            scissor.offset = {x, y};
                            scissor.extent = {static_cast<uint32_t>(width),
                                            static_cast<uint32_t>(height)};

                            vkCmdSetViewport(cmd, 0, 1, &viewport);
                            vkCmdSetScissor(cmd, 0, 1, &scissor);
                        };

                        auto drawModel = [viewIt, drawCount, &frame](
                                            VkCommandBuffer cmd, uint32_t indexCount,
                                            uint32_t descriptorSetCount,
                                            const VkDescriptorSet* descriptorSets,
                                            VkPipelineLayout pipelineLayout)
                        {
                            uint32_t viewBufferOffset = (frame->getDesc().index * drawCount + viewIt) * sizeof(WindowContext::ViewBufferData);
                            vkCmdBindDescriptorSets(
                                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                                descriptorSetCount, descriptorSets, 1, &viewBufferOffset);
                            vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
                        };

                        setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

                        drawModel(
                            cmd, config.resources->model->getDesc().indexCount, config.descriptorSets.size(),
                            config.descriptorSets.data(), config.layout->layout);

                    };

                    drawView(drawIt);

                    drawIt = drawAtomic++;
                }

                commandContext->endCommands();
            });
    }

    // wait for imgui draw job
    {
        ZoneScopedN("waitIMGUI");

        renderIMGUIFuture.get();
    }

    {
        ZoneScopedN("executeCommands");

        auto primaryCommands = primaryCommandContext->commands();

        // TracyVkZone(
        //     primaryCommandContext->userData<command_vulkan::UserData>().tracyContext,
        //     primaryCommands, "executeCommands");

        config.resources->renderTarget->begin(primaryCommands, { VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS });

        // config.resources->renderTarget->clearSingle(primaryCommands, { VK_IMAGE_ASPECT_DEPTH_BIT, 1, { .depthStencil = { 1.0f, 0 } } });
        // config.resources->renderTarget->setDepthStencilAttachmentLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD);
        
        // views
        for (uint32_t contextIt = 1; contextIt <= drawThreadCount; contextIt++)
            primaryCommandContext->execute(*commandContexts()[frameIndex][contextIt]);

        config.resources->renderTarget->nextSubpass(primaryCommands, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

        // ui
        primaryCommandContext->execute(*commandContexts()[frameIndex][0]);

        config.resources->renderTarget->end(primaryCommands);

        VkOffset3D blitSize;
        blitSize.x = config.resources->renderTarget->getRenderTargetDesc().imageExtent.width;
        blitSize.y = config.resources->renderTarget->getRenderTargetDesc().imageExtent.height;
        blitSize.z = 1;
        
        VkImageBlit imageBlitRegion{};
        imageBlitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlitRegion.srcSubresource.layerCount = 1;
        imageBlitRegion.srcOffsets[1] = blitSize;
        imageBlitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlitRegion.dstSubresource.layerCount = 1;
        imageBlitRegion.dstOffsets[1] = blitSize;

        transitionImageLayout(
            primaryCommands,
            config.resources->renderTarget->getRenderTargetDesc().colorImages[0],
            config.resources->renderTarget->getRenderTargetDesc().colorImageFormats[0],
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        transitionImageLayout(
            primaryCommands,
            frame->getDesc().colorImages[0],
            frame->getDesc().colorImageFormats[0],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        vkCmdBlitImage(
            primaryCommands,
            config.resources->renderTarget->getRenderTargetDesc().colorImages[0],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            frame->getDesc().colorImages[0],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &imageBlitRegion,
            VK_FILTER_NEAREST);

        transitionImageLayout(
            primaryCommands,
            config.resources->renderTarget->getRenderTargetDesc().colorImages[0],
            config.resources->renderTarget->getRenderTargetDesc().colorImageFormats[0],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        transitionImageLayout(
            primaryCommands,
            frame->getDesc().colorImages[0],
            frame->getDesc().colorImageFormats[0],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }

    {
        ZoneScopedN("waitViewBuffer");

        updateViewBufferFuture.get();
    }

    // Submit primary command buffer
    uint64_t submitTimelineValue = 0;
    {
        ZoneScopedN("submitCommands");

        SemaphoreHandle<GraphicsBackend::Vulkan> waitSemaphores[2] = {
            myDevice->getTimelineSemaphore(), lastFrame->getNewImageAcquiredSemaphore() };
        Flags<GraphicsBackend::Vulkan> waitDstStageMasks[2] = {
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        uint64_t waitTimelineValues[2] = { waitTimelineValue, 1 };
        SemaphoreHandle<GraphicsBackend::Vulkan> signalSemaphores[2] = {
            myDevice->getTimelineSemaphore(), frame->getRenderCompleteSemaphore() };
        uint64_t signalTimelineValues[2] = { 1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed), 1 };
        submitTimelineValue = primaryCommandContext->submit({
            myDevice->getPrimaryGraphicsQueue(),
            2,
            waitSemaphores,
            waitDstStageMasks,
            waitTimelineValues,
            2,
            signalSemaphores,
            signalTimelineValues});

        frame->setLastSubmitTimelineValue(submitTimelineValue); // todo: remove
    }
    
    return submitTimelineValue;
}

template <>
void WindowContext<GraphicsBackend::Vulkan>::presentFrame(uint32_t frameIndex) const
{
    ZoneScopedN("presentFrame");

    auto& frame = myFrames[frameIndex];

    VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &frame->getRenderCompleteSemaphore();
    info.swapchainCount = 1;
    info.pSwapchains = &mySwapchain->getSwapchain();
    info.pImageIndices = &frameIndex;
    checkFlipOrPresentResult(vkQueuePresentKHR(myDevice->getPrimaryGraphicsQueue(), &info));
}

template <>
void WindowContext<GraphicsBackend::Vulkan>::updateInput(const InputState& input, uint32_t frameIndex, uint32_t lastFrameIndex)
{
    ZoneScopedN("updateInput");

    // todo: unify all keyboard and mouse input. rely on imgui instead of glfw internally.
    {
        using namespace ImGui;

        auto& io = GetIO();
        if (io.WantCaptureMouse)
            return;
    }

    assert(frameIndex < myFrameTimestamps.size());
    assert(lastFrameIndex < myFrameTimestamps.size());

    auto& frameTimestamp = myFrameTimestamps[frameIndex];
    auto& lastFrameTimestamp = myFrameTimestamps[lastFrameIndex];

    float dt = (frameTimestamp - lastFrameTimestamp).count();

    if (input.mouseButtonsPressed[2])
    {
        // todo: generic view index calculation
        size_t viewIdx = input.mousePosition[0].x / (myDesc.windowExtent.width / myDesc.splitScreenGrid.width);
        size_t viewIdy = input.mousePosition[0].y / (myDesc.windowExtent.height / myDesc.splitScreenGrid.height);
        myActiveView = std::min((viewIdy * myDesc.splitScreenGrid.width) + viewIdx, myViews.size() - 1);

        //std::cout << *myActiveView << ":[" << input.mousePosition[0].x << ", " << input.mousePosition[0].y << "]" << std::endl;
    }
    else if (!input.mouseButtonsPressed[0])
    {
        myActiveView.reset();

        //std::cout << "myActiveView.reset()" << std::endl;
    }

    if (myActiveView)
    {
        //std::cout << "window.myActiveView read/consume" << std::endl;

        float dx = 0;
        float dz = 0;

        for (const auto& [key, pressed] : input.keysPressed)
        {
            if (pressed)
            {
                switch (key)
                {
                case GLFW_KEY_W:
                    dz = -1;
                    break;
                case GLFW_KEY_S:
                    dz = 1;
                    break;
                case GLFW_KEY_A:
                    dx = -1;
                    break;
                case GLFW_KEY_D:
                    dx = 1;
                    break;
                default:
                    break;
                }
            }
        }

        auto& view = myViews[*myActiveView];

        bool doUpdateViewMatrix = false;

        if (dx != 0 || dz != 0)
        {
            const auto& viewMatrix = view.getViewMatrix();
            auto forward = glm::vec3(viewMatrix[0][2], viewMatrix[1][2], viewMatrix[2][2]);
            auto strafe = glm::vec3(viewMatrix[0][0], viewMatrix[1][0], viewMatrix[2][0]);

            constexpr auto moveSpeed = 0.000000002f;

            view.desc().cameraPosition += dt * (dz * forward + dx * strafe) * moveSpeed;

            // std::cout << *myActiveView << ":pos:[" << view.desc().cameraPosition.x << ", " <<
            //     view.desc().cameraPosition.y << ", " << view.desc().cameraPosition.z << "]" << std::endl;

            doUpdateViewMatrix = true;
        }

        if (input.mouseButtonsPressed[0])
        {
            constexpr auto rotSpeed = 0.00000001f;

            auto dM = input.mousePosition[1] - input.mousePosition[0];

            view.desc().cameraRotation +=
                dt * glm::vec3(dM.y / view.desc().viewport.height, dM.x / view.desc().viewport.width, 0.0f) *
                rotSpeed;

            // std::cout << *myActiveView << ":rot:[" << view.desc().cameraRotation.x << ", " <<
            //     view.desc().cameraRotation.y << ", " << view.desc().cameraRotation.z << "]" << std::endl;

            doUpdateViewMatrix = true;
        }

        if (doUpdateViewMatrix)
        {
            myViews[*myActiveView].updateViewMatrix();
        }
    }
}

template <>
WindowContext<GraphicsBackend::Vulkan>::WindowContext(
    const std::shared_ptr<InstanceContext<GraphicsBackend::Vulkan>>& instanceContext,
	const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    WindowCreateDesc<GraphicsBackend::Vulkan>&& desc)
: myInstance(instanceContext)
, myDevice(deviceContext)
, myDesc(std::move(desc))
{
    ZoneScopedN("Window()");

    createFrameObjects(desc.framebufferExtent);

    for (const auto& frame : myFrames)
    {
        auto& frameCommandContexts = myCommands.emplace_back();
        const auto& frameCommandPools = myDevice->getGraphicsCommandPools()[frame->getDesc().index];

        uint32_t commandContextCount = std::min<uint32_t>(frameCommandPools.size(), myDesc.maxViewCommandContextCount) + 1;
        frameCommandContexts.reserve(commandContextCount);

        for (uint32_t poolIt = 0; poolIt < commandContextCount; poolIt++)
        {
            frameCommandContexts.emplace_back(std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
                myDevice,
                CommandContextCreateDesc<GraphicsBackend::Vulkan>{frameCommandPools[poolIt]}));
        }
    }
}
