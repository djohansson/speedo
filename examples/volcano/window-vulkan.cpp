#include "window.h"
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
void WindowContext<Vk>::createFrameObjects(Extent2d<Vk> frameBufferExtent)
{
    ZoneScopedN("createFrameObjects");
    
    mySwapchain = std::make_shared<SwapchainContext<Vk>>(
        myAppContext->device,
        SwapchainCreateDesc<Vk>{{{"mySwapchain"}, frameBufferExtent}, mySwapchain ? mySwapchain->getSwapchain() : nullptr});

    myViewBuffer = std::make_unique<Buffer<Vk>>(
        myAppContext,
        BufferCreateDesc<Vk>{
            {"myViewBuffer"},
            myAppContext->device->getDesc().swapchainConfig->imageCount * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height) * sizeof(WindowContext::ViewBufferData),
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
}

template <>
void WindowContext<Vk>::updateViewBuffer(uint32_t frameIndex) const
{
    ZoneScopedN("updateViewBuffer");

    assert(frameIndex < mySwapchain->getFrames().size());

    void* data;
    VK_CHECK(vmaMapMemory(myAppContext->device->getAllocator(), myViewBuffer->getBufferMemory(), &data));

    for (uint32_t n = 0; n < (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height); n++)
    {
        ViewBufferData& ubo = reinterpret_cast<ViewBufferData*>(data)[frameIndex * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height) + n];

        ubo.viewProj = myViews[n].getProjectionMatrix() * glm::mat4(myViews[n].getViewMatrix());
    }

    vmaFlushAllocation(
        myAppContext->device->getAllocator(),
        myViewBuffer->getBufferMemory(),
        sizeof(ViewBufferData) * frameIndex * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height),
        sizeof(ViewBufferData) * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height));

    vmaUnmapMemory(myAppContext->device->getAllocator(), myViewBuffer->getBufferMemory());
}

template <>
void WindowContext<Vk>::draw(const std::shared_ptr<PipelineContext<Vk>>& pipeline)
{
    ZoneScopedN("WindowContext::draw()");

    // std::vector<std::future<void>> drawCallbackFutures;
    // drawCallbackFutures.reserve(myDrawCallbacks.size());
    // for (auto drawCallback : myDrawCallbacks)
    // {
    //     auto& commandContext = myCommands[frameIndex][commandContextIndex];
    //     auto cmd = commandContext->commands(std::move(beginInfo));

    // }
    
    // std::packaged_task<DrawCallback> drawCallbackTask([this](CommandBufferHandle<Vk> cmd)
    // {
    //     ZoneScopedN("drawCallbacks");

    //     for (auto callback : myDrawCallbacks)
    //         callback(cmd);
    // });

    

    // std::future<void> drawCallbacksTaskFuture(
    //     std::async(
    //         std::launch::async,
    //         [&frame, &drawPrepareContextTask, &drawCallbacksTask]
    //         {
    //             drawPrepareContextTask(beginInfo, frame.getDesc().index, 0);
    //             drawCallbacksTask(drawPrepareContextTask.get_future().get());
    //         }));

    //using DrawPrepare = CommandBufferHandle<Vk>(CommandContextBeginInfo<Vk> beginInfo, uint32_t frameIndex, uint32_t commandContextIndex);

    auto frameIndex = mySwapchain->getFrameIndex();
    auto& frame = *mySwapchain->getFrames()[frameIndex];
    auto& commandContext = myCommands[frameIndex][0];

    for (auto [beginInfo, drawCallback] : myDrawCallbacks)
        drawCallback(commandContext->commands(beginInfo));

    myDrawCallbacks.clear();

    pipeline->resources()->renderTarget->transitionColor(commandContext->commands(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);

    auto renderPass = pipeline->resources()->renderTarget->renderPass();
    auto framebuffer = pipeline->resources()->renderTarget->framebuffer();
    auto imageExtent = pipeline->resources()->renderTarget->getRenderTargetDesc().imageExtent;

    std::future<void> updateViewBufferFuture(std::async(std::launch::async, [this, frameIndex]
    {
        updateViewBuffer(frameIndex);
    }));
    
    // setup draw parameters
    uint32_t drawCount = myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height;
    uint32_t drawCommandContextCount = static_cast<uint32_t>(myCommands[frameIndex].size());
    uint32_t drawThreadCount = std::min<uint32_t>(drawCount, drawCommandContextCount);

    std::atomic_uint32_t drawAtomic = 0;

    // draw views using secondary command buffers
    // todo: generalize this to other types of draws
    {
        ZoneScopedN("drawViews");

        std::array<uint32_t, 128> seq;
        std::iota(seq.begin(), seq.begin() + drawCommandContextCount, 0);
        std::for_each_n(
#if defined(__WINDOWS__)
            std::execution::par,
#endif
            seq.begin(), drawThreadCount,
            [this, &pipeline, &renderPass, &framebuffer, &imageExtent, &frameIndex, &drawAtomic, &drawCount](uint32_t threadIt)
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
                
                CommandBufferInheritanceInfo<Vk> inheritInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
                inheritInfo.renderPass = renderPass;
                inheritInfo.framebuffer = framebuffer;

                CommandContextBeginInfo<Vk> beginInfo = {};
                beginInfo.flags =
                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                    VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
                beginInfo.pInheritanceInfo = &inheritInfo;
                beginInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                    
                auto& commandContext = myCommands[frameIndex][threadIt];
                auto cmd = commandContext->commands(beginInfo);

                // TracyVkZone(
                //     commandContext->userData<command_vulkan::UserData>().tracyContext,
                //     cmd, drawPartitionStr.data());
            
                // bind pipeline and inputs
                {
                    // bind pipeline and vertex/index buffers
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getPipeline());

                    VkBuffer vertexBuffers[] = { pipeline->resources()->model->getBuffer().getBufferHandle() };
                    VkDeviceSize vertexOffsets[] = { pipeline->resources()->model->getVertexOffset() };

                    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
                    vkCmdBindIndexBuffer(cmd, pipeline->resources()->model->getBuffer().getBufferHandle(),
                        pipeline->resources()->model->getIndexOffset(), VK_INDEX_TYPE_UINT32);
                }

                uint32_t dx = imageExtent.width / myDesc.splitScreenGrid.width;
                uint32_t dy = imageExtent.height / myDesc.splitScreenGrid.height;

                while (drawIt < drawCount)
                {
                    auto drawView = [this, &drawCount, &frameIndex, &pipeline, &cmd, &dx, &dy](uint32_t viewIt)
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

                        auto drawModel = [viewIt, drawCount, &frameIndex](
                                            VkCommandBuffer cmd, uint32_t indexCount,
                                            uint32_t descriptorSetCount,
                                            const VkDescriptorSet* descriptorSets,
                                            VkPipelineLayout pipelineLayout)
                        {
                            uint32_t viewBufferOffset = (frameIndex * drawCount + viewIt) * sizeof(WindowContext::ViewBufferData);
                            vkCmdBindDescriptorSets(
                                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                                descriptorSetCount, descriptorSets, 1, &viewBufferOffset);
                            vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
                        };

                        setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

                        drawModel(
                            cmd,
                            pipeline->resources()->model->getDesc().indexCount,
                            pipeline->descriptorSets()->size(),
                            pipeline->descriptorSets()->data(),
                            pipeline->layout()->getLayout());

                    };

                    drawView(drawIt);

                    drawIt = drawAtomic++;
                }
            });
    }

    // {
    //     ZoneScopedN("waitDrawCallbacks");

    //     drawCallbacksTaskFuture.get();
    // }

    {
        ZoneScopedN("executeCommands");

        // TracyVkZone(
        //     commandContext->userData<command_vulkan::UserData>().tracyContext,
        //     cmd, "executeCommands");


        // config.resources->renderTarget->clearSingle(cmd, { VK_IMAGE_ASPECT_DEPTH_BIT, 1, { .depthStencil = { 1.0f, 0 } } });
        // config.resources->renderTarget->setDepthStencilAttachmentLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD);

        // {
        //     ZoneScopedN("tracyVkCollect");

            // auto& commandContext = myCommands[frameIndex][0];
            // auto cmd = commandContext->beginScope();

            // TracyVkCollect(
            //     commandContext->userData<command_vulkan::UserData>().tracyContext,
            //     cmd);
        // }

        auto cmd = commandContext->commands();
        pipeline->resources()->renderTarget->begin(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        
        // views
        for (uint32_t contextIt = 0; contextIt < drawThreadCount; contextIt++)
            commandContext->execute(*myCommands[frameIndex][contextIt]);

        pipeline->resources()->renderTarget->nextSubpass(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        pipeline->resources()->renderTarget->end(cmd);

        VkOffset3D blitSize;
        blitSize.x = pipeline->resources()->renderTarget->getRenderTargetDesc().imageExtent.width;
        blitSize.y = pipeline->resources()->renderTarget->getRenderTargetDesc().imageExtent.height;
        blitSize.z = 1;
        
        VkImageBlit imageBlitRegion{};
        imageBlitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlitRegion.srcSubresource.layerCount = 1;
        imageBlitRegion.srcOffsets[1] = blitSize;
        imageBlitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlitRegion.dstSubresource.layerCount = 1;
        imageBlitRegion.dstOffsets[1] = blitSize;

        pipeline->resources()->renderTarget->transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);
        frame.transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0);

        vkCmdBlitImage(
            cmd,
            pipeline->resources()->renderTarget->getRenderTargetDesc().colorImages[0],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            frame.getDesc().colorImages[0],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &imageBlitRegion,
            VK_FILTER_NEAREST);
    }

    {
        ZoneScopedN("waitViewBuffer");

        updateViewBufferFuture.get();
    }
}

template <>
void WindowContext<Vk>::updateInput(const InputState& input)
{
    ZoneScopedN("updateInput");

    // todo: unify all keyboard and mouse input. rely on imgui instead of glfw internally.
    {
        using namespace ImGui;

        auto& io = GetIO();
        if (io.WantCaptureMouse)
            return;
    }

    myTimestamps[1] = myTimestamps[0];
    myTimestamps[0] = std::chrono::high_resolution_clock::now();

    float dt = (myTimestamps[1] - myTimestamps[0]).count();

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
WindowContext<Vk>::WindowContext(
    const std::shared_ptr<ApplicationContext<Vk>>& appContext,
    WindowCreateDesc<Vk>&& desc)
: myAppContext(appContext)
, myDesc(std::move(desc))
{
    ZoneScopedN("Window()");

    createFrameObjects(desc.framebufferExtent);

    const auto& graphicsCommandPools = myAppContext->device->getGraphicsCommandPools();

    uint32_t frameCount = mySwapchain->getFrames().size();
    uint32_t commandContextCount = std::min<uint32_t>(graphicsCommandPools.size(), myDesc.maxCommandContextPerFrameCount * frameCount);
    assert(commandContextCount >= frameCount);
    uint32_t commandContextPerFrameCount = commandContextCount / frameCount;

    myCommands.resize(frameCount);

    for (uint32_t frameIt = 0; frameIt < frameCount; frameIt++)
    {
        auto& commandContexts = myCommands[frameIt];
        
        commandContexts.reserve(commandContextPerFrameCount);

        for (uint32_t poolIt = 0; poolIt < commandContextPerFrameCount; poolIt++)
        {
            commandContexts.emplace_back(std::make_shared<CommandContext<Vk>>(
                myAppContext->device,
                CommandContextCreateDesc<Vk>{{"CommandContext"}, graphicsCommandPools[frameIt * commandContextPerFrameCount + poolIt]}));
        }
    }

    myTimestamps[0] = std::chrono::high_resolution_clock::now();
}
