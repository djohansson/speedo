#include "window.h"
#include "gfx.h"

#if defined(__WINDOWS__)
#include <execution>
#endif
#include <filesystem>
#include <future>
#include <numeric>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <examples/imgui_impl_vulkan.h>


template <>
void Window<GraphicsBackend::Vulkan>::createFrameObjects(CommandContext<GraphicsBackend::Vulkan>& commandContext)
{
    mySwapchainContext = std::make_unique<SwapchainContext<GraphicsBackend::Vulkan>>(
        SwapchainDesc<GraphicsBackend::Vulkan>{
            myWindowDesc.deviceContext,
            nullptr,
            myWindowDesc.framebufferExtent});

    // todo: append stencil bit for depthstencil composite formats
    myDepthTexture = std::make_unique<Texture<GraphicsBackend::Vulkan>>(
        TextureDesc<GraphicsBackend::Vulkan>{
            myWindowDesc.deviceContext,
            myWindowDesc.framebufferExtent,
            1,
            0,
            findSupportedFormat(
                myWindowDesc.deviceContext->getPhysicalDevice(),
                {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE, 
            "myDepthTexture"},
        commandContext);

    myViewBuffer = std::make_unique<Buffer<GraphicsBackend::Vulkan>>(
        BufferDesc<GraphicsBackend::Vulkan>{
            myWindowDesc.deviceContext,
            myWindowDesc.deviceContext->getSwapchainConfiguration().selectedImageCount * (NX * NY) * sizeof(Window::ViewBufferData),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            "myViewBuffer"},
        commandContext);

    myViews.resize(NX * NY);
    for (auto& view : myViews)
    {
        if (!view.viewDesc().viewport.width)
            view.viewDesc().viewport.width =  myWindowDesc.framebufferExtent.width / NX;

        if (!view.viewDesc().viewport.height)
            view.viewDesc().viewport.height = myWindowDesc.framebufferExtent.height / NY;

        view.updateAll();
    }

    myRenderPass = createRenderPass(
        myWindowDesc.deviceContext->getDevice(),
        myWindowDesc.deviceContext->getSwapchainConfiguration().selectedFormat().format,
        myDepthTexture->getTextureDesc().format);

    uint32_t imageCount;
    CHECK_VK(vkGetSwapchainImagesKHR(myWindowDesc.deviceContext->getDevice(), mySwapchainContext->getSwapchain(), &imageCount, nullptr));
    
    std::vector<ImageHandle<GraphicsBackend::Vulkan>> colorImages(imageCount);
    CHECK_VK(vkGetSwapchainImagesKHR(myWindowDesc.deviceContext->getDevice(), mySwapchainContext->getSwapchain(), &imageCount, colorImages.data()));

    uint32_t frameCount = myWindowDesc.deviceContext->getSwapchainConfiguration().selectedImageCount;
    myFrames.reserve(frameCount);
    for (uint32_t frameIt = 0; frameIt < frameCount; frameIt++)
        myFrames.emplace_back(
            RenderTargetDesc<GraphicsBackend::Vulkan>{
                myWindowDesc.deviceContext,
                myRenderPass,
                myWindowDesc.framebufferExtent,
                myWindowDesc.deviceContext->getSwapchainConfiguration().selectedFormat().format,
                1,
                &colorImages[frameIt], 
                myDepthTexture->getTextureDesc().format,
                myDepthTexture->getImage()},
            FrameDesc<GraphicsBackend::Vulkan>{
                myWindowDesc.timelineSemaphore,
                myWindowDesc.timelineValue});

    // vkAcquireNextImageKHR uses semaphore from last frame -> cant use index 0 for first frame
    myFrameIndex = myFrames.size() - 1;
}


template <>
void Window<GraphicsBackend::Vulkan>::destroyFrameObjects()
{
    myFrames.clear();
    mySwapchainContext.reset();
    myDepthTexture.reset();
    myViewBuffer.reset();

    if (myRenderPass)
    {
        vkDestroyRenderPass(myWindowDesc.deviceContext->getDevice(), myRenderPass, nullptr);
        myRenderPass = 0;
    }
}

template <>
void Window<GraphicsBackend::Vulkan>::updateViewBuffer() const
{
    void* data;
    CHECK_VK(vmaMapMemory(myWindowDesc.deviceContext->getAllocator(), myViewBuffer->getBufferMemory(), &data));

    for (uint32_t n = 0; n < (NX * NY); n++)
    {
        ViewBufferData& ubo = reinterpret_cast<ViewBufferData*>(data)[myFrameIndex * (NX * NY) + n];

        ubo.viewProj = myViews[n].getProjectionMatrix() * glm::mat4(myViews[n].getViewMatrix());
    }

    vmaFlushAllocation(
        myWindowDesc.deviceContext->getAllocator(),
        myViewBuffer->getBufferMemory(),
        sizeof(ViewBufferData) * myFrameIndex * (NX * NY),
        sizeof(ViewBufferData) * (NX * NY));

    vmaUnmapMemory(myWindowDesc.deviceContext->getAllocator(), myViewBuffer->getBufferMemory());
}

template <>
void Window<GraphicsBackend::Vulkan>::submitFrame(
    const PipelineConfiguration<GraphicsBackend::Vulkan>& config)
{
    myLastFrameIndex = myFrameIndex;
    auto& lastFrame = myFrames[myLastFrameIndex];

    {
        //ZoneScopedN("acquireNextImage");

        checkFlipOrPresentResult(
            vkAcquireNextImageKHR(
                myWindowDesc.deviceContext->getDevice(),
                mySwapchainContext->getSwapchain(),
                UINT64_MAX,
                lastFrame.getNewImageAcquiredSemaphore(),
                VK_NULL_HANDLE,
                &myFrameIndex));
    }

    auto& frame = myFrames[myFrameIndex];
    
    // wait for frame to be completed before starting to use it
    {
        //ZoneScopedN("waitForFrameFence");

        frame.waitForFence();
    }

    std::future<void> drawIMGUIFuture(std::async(std::launch::async, [this]
    {
        if (myWindowDesc.imguiEnable)
        {
            ImGui_ImplVulkan_NewFrame();
            drawIMGUI();
        }
    }));

    std::future<void> updateViewBufferFuture(std::async(std::launch::async, [this]
    {
        updateViewBuffer();
    }));

    // collect timing scopes
    {
        //ZoneScopedN("tracyVkCollect");
        // TracyVkZone(tracyContext, frame.commands[0], "tracyVkCollect");
        // TracyVkCollect(tracyContext, frame.commands[0]);
    }

    std::array<ClearValue<GraphicsBackend::Vulkan>, 2> clearValues = {};
    clearValues[0] = myWindowDesc.clearValue;
    clearValues[1].depthStencil = {1.0f, 0};

    // create secondary command buffers
    {
        assert(myWindowDesc.deviceContext->getDeviceDesc().commandBufferThreadCount > 1);

        // setup draw parameters
        constexpr uint32_t drawCount = NX * NY;
        uint32_t segmentCount = std::max(static_cast<uint32_t>(myWindowDesc.deviceContext->getDeviceDesc().commandBufferThreadCount) - 1, 1u);

        assert(config.resources);

        // draw geometry using secondary command buffers
        {
            //ZoneScopedN("waitDraw");

            uint32_t segmentDrawCount = drawCount / segmentCount;
            if (drawCount % segmentCount)
                segmentDrawCount += 1;

            uint32_t dx = frame.getRenderTargetDesc().imageExtent.width / NX;
            uint32_t dy = frame.getRenderTargetDesc().imageExtent.height / NY;

            std::array<uint32_t, 128> seq;
            std::iota(seq.begin(), seq.begin() + segmentCount, 0);
            std::for_each_n(
#if defined(__WINDOWS__)
				std::execution::par,
#endif
                seq.begin(), segmentCount,
                [this, &config, &frame, &dx, &dy, &segmentDrawCount](uint32_t segmentIt)
                {
                    
                    CommandBufferInheritanceInfo<GraphicsBackend::Vulkan> inherit = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
                    inherit.renderPass = myRenderPass;
                    inherit.framebuffer = frame.getFrameBuffer();

                    CommandBufferBeginInfo<GraphicsBackend::Vulkan> secBeginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    secBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                                        VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
                                        VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
                    secBeginInfo.pInheritanceInfo = &inherit;
                        
                    auto& secondaryCommandContext = frame.commandContext(segmentIt + 1);
                    {
                        auto cmd = secondaryCommandContext->begin(&secBeginInfo);
                    
                        // bind pipeline and inputs
                        {
                            
                            // bind pipeline and vertex/index buffers
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, config.graphicsPipeline);

                            VkBuffer vertexBuffers[] = {config.resources->model->getVertexBuffer().getBuffer()};
                            VkDeviceSize vertexOffsets[] = {0};

                            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
                            vkCmdBindIndexBuffer(cmd, config.resources->model->getIndexBuffer().getBuffer(), 0, VK_INDEX_TYPE_UINT32);
                        }

                        for (uint32_t drawIt = 0; drawIt < segmentDrawCount; drawIt++)
                        {
                            //ZoneScopedN("draw");
                            //TracyVkZone(myTracyContext, cmd, "draw");
                            
                            uint32_t n = segmentIt * segmentDrawCount + drawIt;

                            if (n >= drawCount)
                                break;

                            uint32_t i = n % NX;
                            uint32_t j = n / NX;

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

                            auto drawModel = [this, &n](
                                                VkCommandBuffer cmd, uint32_t indexCount,
                                                uint32_t descriptorSetCount,
                                                const VkDescriptorSet* descriptorSets,
                                                VkPipelineLayout pipelineLayout) {
                                uint32_t viewBufferOffset = (myFrameIndex * drawCount + n) * sizeof(Window::ViewBufferData);
                                vkCmdBindDescriptorSets(
                                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                                    descriptorSetCount, descriptorSets, 1, &viewBufferOffset);
                                vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
                            };

                            setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

                            drawModel(
                                cmd, config.resources->model->getModelDesc().indexCount, config.descriptorSets.size(),
                                config.descriptorSets.data(), config.layout->layout);
                        }

                        secondaryCommandContext->end();
                    }
                });
        }
    }

    auto& primaryCommandContext = frame.commandContext(0);
    auto primaryCommands = primaryCommandContext->begin();

    // call secondary command buffers
    VkRenderPassBeginInfo beginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    beginInfo.renderPass = myRenderPass;
    beginInfo.framebuffer = frame.getFrameBuffer();
    beginInfo.renderArea.offset = {0, 0};
    beginInfo.renderArea.extent = {frame.getRenderTargetDesc().imageExtent.width, frame.getRenderTargetDesc().imageExtent.height};
    beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    beginInfo.pClearValues = clearValues.data();
    
    primaryCommandContext->execute(*frame.commandContext(1), &beginInfo);

    // wait for imgui draw job
    {
        //ZoneScopedN("waitDrawIMGUI");

        drawIMGUIFuture.get();
    }

    // Record Imgui Draw Data and draw funcs into primary command buffer
    if (myWindowDesc.imguiEnable)
    {
        //ZoneScopedN("drawIMGUI");
        //TracyVkZone(tracyContext, primaryCommands, "drawIMGUI");

        VkRenderPassBeginInfo beginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        beginInfo.renderPass = myRenderPass;
        beginInfo.framebuffer = frame.getFrameBuffer();
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent = {frame.getRenderTargetDesc().imageExtent.width, frame.getRenderTargetDesc().imageExtent.height};
        beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        beginInfo.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(primaryCommands, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), primaryCommands);

        vkCmdEndRenderPass(primaryCommands);
    }

    // Submit primary command buffer
    {
        //ZoneScopedN("waitViewBuffer");

        updateViewBufferFuture.get();
    }

    primaryCommandContext->end();

    {
        //ZoneScopedN("submitCommands");

        Flags<GraphicsBackend::Vulkan> waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        primaryCommandContext->submit(CommandSubmitInfo<GraphicsBackend::Vulkan>{
            myWindowDesc.deviceContext->getSelectedQueue(),
            1,
            &lastFrame.getNewImageAcquiredSemaphore(),
            &waitStage,
            1,
            &frame.getRenderCompleteSemaphore(),
            frame.getFence(),
        });
    }
}

template <>
void Window<GraphicsBackend::Vulkan>::presentFrame() const
{
    auto& frame = myFrames[myFrameIndex];

    VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &frame.getRenderCompleteSemaphore();
    info.swapchainCount = 1;
    info.pSwapchains = &mySwapchainContext->getSwapchain();
    info.pImageIndices = &myFrameIndex;
    checkFlipOrPresentResult(vkQueuePresentKHR(myWindowDesc.deviceContext->getSelectedQueue(), &info));
}

template <>
void Window<GraphicsBackend::Vulkan>::updateInput(const InputState& input)
{
    auto& frame = myFrames[myFrameIndex];
    auto& lastFrame = myFrames[myLastFrameIndex];

    float dt = (frame.getTimestamp() - lastFrame.getTimestamp()).count();

    // update input dependent state
    {
        auto& io = ImGui::GetIO();

        static bool escBufferState = false;
        bool escState = io.KeysDown[io.KeyMap[ImGuiKey_Escape]];
        if (escState && !escBufferState)
            myWindowDesc.imguiEnable = !myWindowDesc.imguiEnable;
        escBufferState = escState;
    }

    if (input.mouseButtonsPressed[2])
    {
        // todo: generic view index calculation
        size_t viewIdx = input.mousePosition[0].x / (myWindowDesc.windowExtent.width / NX);
        size_t viewIdy = input.mousePosition[0].y / (myWindowDesc.windowExtent.height / NY);
        myActiveView = std::min((viewIdy * NX) + viewIdx, myViews.size() - 1);

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
                    dz = 1;
                    break;
                case GLFW_KEY_S:
                    dz = -1;
                    break;
                case GLFW_KEY_A:
                    dx = 1;
                    break;
                case GLFW_KEY_D:
                    dx = -1;
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

            view.viewDesc().cameraPosition += dt * (dz * forward + dx * strafe) * moveSpeed;

            // std::cout << *myActiveView << ":pos:[" << view.viewDesc().cameraPosition.x << ", " <<
            //     view.viewDesc().cameraPosition.y << ", " << view.viewDesc().cameraPosition.z << "]" << std::endl;

            doUpdateViewMatrix = true;
        }

        if (input.mouseButtonsPressed[0])
        {
            constexpr auto rotSpeed = 0.00000001f;

            auto dM = input.mousePosition[0] - input.mousePosition[1];

            view.viewDesc().cameraRotation +=
                dt * glm::vec3(dM.y / view.viewDesc().viewport.height, dM.x / view.viewDesc().viewport.width, 0.0f) *
                rotSpeed;

            // std::cout << *myActiveView << ":rot:[" << view.viewDesc().cameraRotation.x << ", " <<
            //     view.viewDesc().cameraRotation.y << ", " << view.viewDesc().cameraRotation.z << "]" << std::endl;

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
    WindowDesc<GraphicsBackend::Vulkan>&& desc, CommandContext<GraphicsBackend::Vulkan>& commands)
: myWindowDesc(std::move(desc))
{
    createFrameObjects(commands);
}

template <>
Window<GraphicsBackend::Vulkan>::~Window()
{
    destroyFrameObjects();
}
