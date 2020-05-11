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
void Window<GraphicsBackend::Vulkan>::renderIMGUI()
{
    ZoneScopedN("renderIMGUI");
    
    using namespace ImGui;

    ImGui_ImplVulkan_NewFrame();
    NewFrame();

    for (auto callback : myIMGUIDrawCallbacks)
        callback();

    Render();
}

// todo: remove dependency on external CommandContext (since we will not be uploading any data anyways). need to add another constructor to Texture to facilitate
template <>
void Window<GraphicsBackend::Vulkan>::createFrameObjects()
{
    ZoneScopedN("createFrameObjects");

    uint32_t physicalDeviceIndex = myDeviceContext->getDesc().physicalDeviceIndex;
    myInstanceContext->updateSurfaceCapabilities(physicalDeviceIndex);
    auto frameBufferExtent = 
        myInstanceContext->getPhysicalDeviceInfos()[physicalDeviceIndex].swapchainInfo.capabilities.currentExtent;

    mySwapchainContext = std::make_unique<SwapchainContext<GraphicsBackend::Vulkan>>(
        myDeviceContext,
        SwapchainCreateDesc<GraphicsBackend::Vulkan>{
            frameBufferExtent,
            mySwapchainContext ? mySwapchainContext->getSwapchain() : nullptr});

    // todo: append stencil bit for depthstencil composite formats
    myDepthTexture = std::make_unique<Texture<GraphicsBackend::Vulkan>>(
        myDeviceContext,
        TextureCreateDesc<GraphicsBackend::Vulkan>{
            {"myDepthTexture"},
            frameBufferExtent,
            1,
            findSupportedFormat(
                myDeviceContext->getPhysicalDevice(),
                {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});

    myViewBuffer = std::make_unique<Buffer<GraphicsBackend::Vulkan>>(
        myDeviceContext,
        BufferCreateDesc<GraphicsBackend::Vulkan>{
            {"myViewBuffer"},
            myDeviceContext->getDesc().swapchainConfiguration->imageCount * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height) * sizeof(Window::ViewBufferData),
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

    myRenderPass = createRenderPass(
        myDeviceContext->getDevice(),
        myDeviceContext->getDesc().swapchainConfiguration->surfaceFormat.format,
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        myDepthTexture->getDesc().format,
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    myUIRenderPass = createRenderPass(
        myDeviceContext->getDevice(),
        myDeviceContext->getDesc().swapchainConfiguration->surfaceFormat.format,
        VK_ATTACHMENT_LOAD_OP_LOAD,
        VK_ATTACHMENT_STORE_OP_STORE,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        myDepthTexture->getDesc().format,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    uint32_t imageCount;
    CHECK_VK(vkGetSwapchainImagesKHR(myDeviceContext->getDevice(), mySwapchainContext->getSwapchain(), &imageCount, nullptr));
    
    std::vector<ImageHandle<GraphicsBackend::Vulkan>> colorImages(imageCount);
    CHECK_VK(vkGetSwapchainImagesKHR(myDeviceContext->getDevice(), mySwapchainContext->getSwapchain(), &imageCount, colorImages.data()));

    uint32_t frameCount = myDeviceContext->getDesc().swapchainConfiguration->imageCount;
    myFrames.reserve(frameCount);
    myFrameTimestamps.resize(frameCount);
    for (uint32_t frameIt = 0; frameIt < frameCount; frameIt++)
        myFrames.emplace_back(std::make_shared<Frame<GraphicsBackend::Vulkan>>(
            myDeviceContext,
            FrameCreateDesc<GraphicsBackend::Vulkan>{
                {myRenderPass,
                frameBufferExtent,
                myDeviceContext->getDesc().swapchainConfiguration->surfaceFormat.format,
                {colorImages[frameIt]},
                myDepthTexture->getDesc().format,
                myDepthTexture->getImage()},
                frameIt}));
}

template <>
void Window<GraphicsBackend::Vulkan>::destroyFrameObjects()
{
    ZoneScopedN("windowDestroyFrameObjects");

    myFrames.clear();
    myFrameTimestamps.clear();
    mySwapchainContext.reset();
    myDepthTexture.reset();
    myViewBuffer.reset();

    if (myRenderPass)
    {
        vkDestroyRenderPass(myDeviceContext->getDevice(), myRenderPass, nullptr);
        myRenderPass = 0;
    }

    if (myUIRenderPass)
    {
        vkDestroyRenderPass(myDeviceContext->getDevice(), myUIRenderPass, nullptr);
        myUIRenderPass = 0;
    }
}

template <>
void Window<GraphicsBackend::Vulkan>::updateViewBuffer(uint32_t frameIndex) const
{
    ZoneScopedN("updateViewBuffer");

    assert(frameIndex < myFrames.size());

    void* data;
    CHECK_VK(vmaMapMemory(myDeviceContext->getAllocator(), myViewBuffer->getBufferMemory(), &data));

    for (uint32_t n = 0; n < (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height); n++)
    {
        ViewBufferData& ubo = reinterpret_cast<ViewBufferData*>(data)[frameIndex * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height) + n];

        ubo.viewProj = myViews[n].getProjectionMatrix() * glm::mat4(myViews[n].getViewMatrix());
    }

    vmaFlushAllocation(
        myDeviceContext->getAllocator(),
        myViewBuffer->getBufferMemory(),
        sizeof(ViewBufferData) * frameIndex * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height),
        sizeof(ViewBufferData) * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height));

    vmaUnmapMemory(myDeviceContext->getAllocator(), myViewBuffer->getBufferMemory());
}

template <>
std::tuple<bool, uint32_t> Window<GraphicsBackend::Vulkan>::flipFrame(uint32_t lastFrameIndex) const
{
    ZoneScoped;

    static constexpr std::string_view flipFrameStr = "flipFrame";

    auto& lastFrame = myFrames[lastFrameIndex];

    uint32_t frameIndex;
    auto flipResult = checkFlipOrPresentResult(
        vkAcquireNextImageKHR(
            myDeviceContext->getDevice(),
            mySwapchainContext->getSwapchain(),
            UINT64_MAX,
            lastFrame->getNewImageAcquiredSemaphore(),
            0,
            &frameIndex));

    if (flipResult != VK_SUCCESS)
    {
        assert(lastFrameIndex == frameIndex); // just check that vkAcquireNextImageKHR is not messing up things

        static constexpr std::string_view errorStr = " - ERROR: vkAcquireNextImageKHR failed";

        char failedStr[flipFrameStr.size() + errorStr.size() + 1];
        sprintf_s(failedStr, sizeof(failedStr), "%.*s%.*s",
            static_cast<int>(flipFrameStr.size()), flipFrameStr.data(),
            static_cast<int>(errorStr.size()), errorStr.data());

        ZoneName(failedStr, sizeof_array(failedStr));

        // todo: print error code
        //ZoneText(failedStr, sizeof_array(failedStr));

        return std::make_tuple(false, frameIndex);
    }

    char flipFrameWithNumberStr[flipFrameStr.size()+2];
    sprintf_s(flipFrameWithNumberStr, sizeof(flipFrameWithNumberStr), "%.*s%u",
        static_cast<int>(flipFrameStr.size()), flipFrameStr.data(), frameIndex);

    ZoneName(flipFrameWithNumberStr, sizeof_array(flipFrameWithNumberStr));
    
    return std::make_tuple(true, frameIndex);
}

template <>
uint64_t Window<GraphicsBackend::Vulkan>::submitFrame(
    uint32_t frameIndex,
    uint32_t lastFrameIndex,
    const PipelineConfiguration<GraphicsBackend::Vulkan>& config,
    uint64_t waitTimelineValue)
{
    ZoneScopedN("submitFrame");

    myFrameTimestamps[frameIndex] = std::chrono::high_resolution_clock::now();

    auto& frame = myFrames[frameIndex];
    auto& lastFrame = myFrames[lastFrameIndex];

    std::future<void> renderIMGUIFuture(std::async(std::launch::async, [this]
    {
        renderIMGUI();
    }));

    std::future<void> updateViewBufferFuture(std::async(std::launch::async, [this, frameIndex]
    {
        updateViewBuffer(frameIndex);
    }));

    {
        ZoneScopedN("tracyVkCollect");

        for (uint32_t contextIt = 0; contextIt < 1/*frame->commandContexts().size()*/; contextIt++)
        {
            auto& commandContext = commandContexts()[frameIndex][contextIt];
            auto cmd = commandContext->beginEndScope();

            TracyVkZone(
                commandContext->userData<command_vulkan::UserData>().tracyContext,
                cmd, "tracyVkCollect");

            TracyVkCollect(
                commandContext->userData<command_vulkan::UserData>().tracyContext,
                cmd);
        }
    }

    std::array<ClearValue<GraphicsBackend::Vulkan>, 2> clearValues = {};
    clearValues[0] = myDesc.clearValue;
    clearValues[1].depthStencil = {1.0f, 0};

    // setup draw parameters
    uint32_t drawCount = myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height;
    uint32_t commandContextCount = static_cast<uint32_t>(commandContexts()[frameIndex].size());
    uint32_t segmentCount = std::max<uint32_t>(std::min<int32_t>(drawCount, commandContextCount - 1), 0);

    // draw geometry using secondary command buffers
    {
        ZoneScopedN("drawGeometry");

        uint32_t segmentDrawCount = drawCount / segmentCount;
        if (drawCount % segmentCount)
            segmentDrawCount += 1;

        uint32_t dx = frame->getDesc().imageExtent.width / myDesc.splitScreenGrid.width;
        uint32_t dy = frame->getDesc().imageExtent.height / myDesc.splitScreenGrid.height;

        std::array<uint32_t, 128> seq;
        std::iota(seq.begin(), seq.begin() + segmentCount, 0);
        std::for_each_n(
#if defined(__WINDOWS__)
             std::execution::par,
#endif
            seq.begin(), segmentCount,
            [this, &config, &frame, &dx, &dy, &drawCount, &segmentDrawCount](uint32_t segmentIt)
            {
                ZoneScoped;

                static constexpr std::string_view drawSegmentStr = "drawSegment";
                char drawSegmentWithNumberStr[drawSegmentStr.size()+3];
                sprintf_s(drawSegmentWithNumberStr, sizeof(drawSegmentWithNumberStr), "%.*s%u",
                    static_cast<int>(drawSegmentStr.size()), drawSegmentStr.data(), segmentIt);

                ZoneName(drawSegmentWithNumberStr, sizeof_array(drawSegmentWithNumberStr));
                
                CommandBufferInheritanceInfo<GraphicsBackend::Vulkan> inherit = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
                inherit.renderPass = myRenderPass;
                inherit.framebuffer = frame->getFrameBuffer();

                CommandBufferBeginInfo<GraphicsBackend::Vulkan> secBeginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                secBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                                    VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
                secBeginInfo.pInheritanceInfo = &inherit;
                    
                auto& commandContext = commandContexts()[frame->getDesc().index][segmentIt + 1];
                auto cmd = commandContext->beginEndScope(&secBeginInfo);

                // TracyVkZone(
                //     commandContext->userData<command_vulkan::UserData>().tracyContext,
                //     cmd, drawSegmentStr.data());
            
                // bind pipeline and inputs
                {
                    
                    // bind pipeline and vertex/index buffers
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, config.graphicsPipeline);

                    VkBuffer vertexBuffers[] = {config.resources->model->getBuffer().getBuffer()};
                    VkDeviceSize vertexOffsets[] = {config.resources->model->getVertexOffset()};

                    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
                    vkCmdBindIndexBuffer(cmd, config.resources->model->getBuffer().getBuffer(),
                        config.resources->model->getIndexOffset(), VK_INDEX_TYPE_UINT32);
                }

                for (uint32_t drawIt = 0; drawIt < segmentDrawCount; drawIt++)
                {
                    uint32_t n = segmentIt * segmentDrawCount + drawIt;

                    if (n >= drawCount)
                        break;

                    uint32_t i = n % myDesc.splitScreenGrid.width;
                    uint32_t j = n / myDesc.splitScreenGrid.width;

                    auto setViewportAndScissor = [](VkCommandBuffer cmd, int32_t x, int32_t y,
                                                    int32_t width, int32_t height) {
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

                    auto drawModel = [n, drawCount, &frame](
                                        VkCommandBuffer cmd, uint32_t indexCount,
                                        uint32_t descriptorSetCount,
                                        const VkDescriptorSet* descriptorSets,
                                        VkPipelineLayout pipelineLayout) {
                        uint32_t viewBufferOffset = (frame->getDesc().index * drawCount + n) * sizeof(Window::ViewBufferData);
                        vkCmdBindDescriptorSets(
                            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                            descriptorSetCount, descriptorSets, 1, &viewBufferOffset);
                        vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
                    };

                    setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

                    drawModel(
                        cmd, config.resources->model->getDesc().indexCount, config.descriptorSets.size(),
                        config.descriptorSets.data(), config.layout->layout);
                }
            });
    }

    auto& primaryCommandContext = commandContexts()[frameIndex][0];

    {
        ZoneScopedN("executeCommands");

        auto primaryCommands = primaryCommandContext->beginEndScope();

        // call secondary command buffers
        VkRenderPassBeginInfo beginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        beginInfo.renderPass = myRenderPass;
        beginInfo.framebuffer = frame->getFrameBuffer();
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent = {frame->getDesc().imageExtent.width, frame->getDesc().imageExtent.height};
        beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        beginInfo.pClearValues = clearValues.data();

        TracyVkZone(
            primaryCommandContext->userData<command_vulkan::UserData>().tracyContext,
            primaryCommands, "executeCommands");

        vkCmdBeginRenderPass(primaryCommands, &beginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

        for (uint32_t contextIt = 1; contextIt <= segmentCount; contextIt++)
        {
            primaryCommandContext->execute(*commandContexts()[frameIndex][contextIt]);
            //vkCmdNextSubpass(primaryCommands, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        }

        vkCmdEndRenderPass(primaryCommands);
    }

    // wait for imgui draw job
    {
        ZoneScopedN("waitIMGUI");

        renderIMGUIFuture.get();
    }

    // Record Imgui Draw Data and draw funcs into primary command buffer
    {
        auto primaryCommands = primaryCommandContext->beginEndScope();
        
        ZoneScopedN("drawIMGUI");

        TracyVkZone(
            primaryCommandContext->userData<command_vulkan::UserData>().tracyContext,
            primaryCommands, "drawIMGUI");

        VkRenderPassBeginInfo beginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        beginInfo.renderPass = myUIRenderPass;
        beginInfo.framebuffer = frame->getFrameBuffer();
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent = {frame->getDesc().imageExtent.width, frame->getDesc().imageExtent.height};
        beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        beginInfo.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(primaryCommands, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), primaryCommands);

        vkCmdEndRenderPass(primaryCommands);
    }

    // Submit primary command buffer
    {
        ZoneScopedN("waitViewBuffer");

        updateViewBufferFuture.get();
    }

    uint64_t submitResult = 0;
    {
        ZoneScopedN("submitCommands");

        SemaphoreHandle<GraphicsBackend::Vulkan> waitSemaphores[2] = {
            lastFrame->getNewImageAcquiredSemaphore(), myDeviceContext->getTimelineSemaphore() };
        Flags<GraphicsBackend::Vulkan> waitStages[2] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
        uint64_t waitTimelineValues[2] = {
            waitTimelineValue, waitTimelineValue};
        submitResult = primaryCommandContext->submit({
            myDeviceContext->getPrimaryGraphicsQueue(),
            frame->getFence(),
            1,
            &frame->getRenderCompleteSemaphore(),
            2,
            waitSemaphores,
            waitStages,
            waitTimelineValues, 
        });
        frame->setLastSubmitTimelineValue(submitResult);
    }
    
    return submitResult;
}

template <>
void Window<GraphicsBackend::Vulkan>::presentFrame(uint32_t frameIndex) const
{
    ZoneScopedN("presentFrame");

    auto& frame = myFrames[frameIndex];

    VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &frame->getRenderCompleteSemaphore();
    info.swapchainCount = 1;
    info.pSwapchains = &mySwapchainContext->getSwapchain();
    info.pImageIndices = &frameIndex;
    checkFlipOrPresentResult(vkQueuePresentKHR(myDeviceContext->getPrimaryGraphicsQueue(), &info));
}

template <>
void Window<GraphicsBackend::Vulkan>::updateInput(const InputState& input, uint32_t frameIndex, uint32_t lastFrameIndex)
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
Window<GraphicsBackend::Vulkan>::Window(
    const std::shared_ptr<InstanceContext<GraphicsBackend::Vulkan>>& instanceContext,
	const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    WindowCreateDesc<GraphicsBackend::Vulkan>&& desc)
: myInstanceContext(instanceContext)
, myDeviceContext(deviceContext)
, myDesc(std::move(desc))
{
    ZoneScopedN("Window()");

    createFrameObjects();

    for (const auto& frame : myFrames)
    {
        auto& frameCommandContexts = myCommandContexts.emplace_back();
        const auto& frameCommandPools = myDeviceContext->getGraphicsCommandPools()[frame->getDesc().index];

        uint32_t commandContextCount = std::min<uint32_t>(frameCommandPools.size(), myDesc.maxCommandContextCount);
        frameCommandContexts.reserve(commandContextCount);

        for (uint32_t poolIt = 0; poolIt < commandContextCount; poolIt++)
        {
            frameCommandContexts.emplace_back(std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
               myDeviceContext,
                CommandContextCreateDesc<GraphicsBackend::Vulkan>{
                    frameCommandPools[poolIt],
                    static_cast<uint32_t>(poolIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY)}));
        }

        frameCommandContexts[0]->userData<command_vulkan::UserData>().tracyContext =
            TracyVkContext(
                myDeviceContext->getPhysicalDevice(),
                myDeviceContext->getDevice(),
                myDeviceContext->getPrimaryGraphicsQueue(),
                frameCommandContexts[0]->commands());
    }
}

template <>
Window<GraphicsBackend::Vulkan>::~Window()
{
    ZoneScopedN("~Window()");

    destroyFrameObjects();

    for (auto& frameCommandContexts : myCommandContexts)
        for (auto& commandContext : frameCommandContexts)
            commandContext->clear();
}
