#include "window.h"
#include "vk-utils.h"

#include <stb_sprintf.h>

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
    ZoneScopedN("WindowContext::createFrameObjects");
    
    mySwapchain = std::make_shared<Swapchain<Vk>>(
        myDevice,
        SwapchainCreateDesc<Vk>{{{"mySwapchain"}, frameBufferExtent}, mySwapchain ? mySwapchain->getSwapchain() : nullptr});

    myViewBuffer = std::make_unique<Buffer<Vk>>(
        myDevice,
        BufferCreateDesc<Vk>{
            {"myViewBuffer"},
            myDevice->getDesc().swapchainConfig->imageCount * (myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height) * sizeof(WindowContext::ViewBufferData),
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
    ZoneScopedN("WindowContext::updateViewBuffer");

    assert(frameIndex < mySwapchain->getFrames().size());

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
uint32_t WindowContext<Vk>::internalDrawViews(
    const std::shared_ptr<PipelineContext<Vk>>& pipeline,
    const RenderPassBeginInfo<Vk>& renderPassInfo,
    uint32_t frameIndex)
{
    // setup draw parameters
    uint32_t drawCount = myDesc.splitScreenGrid.width * myDesc.splitScreenGrid.height;
    uint32_t drawCommandContextCount = static_cast<uint32_t>(myCommands[frameIndex].size());
    uint32_t drawThreadCount = std::min<uint32_t>(drawCount, drawCommandContextCount);

    std::atomic_uint32_t drawAtomic = 0;
    
    // draw views using secondary command buffers
    // todo: generalize this to other types of draws
    if (pipeline->resources().model)
    {
        ZoneScopedN("WindowContext::drawViews");

        std::array<uint32_t, 128> seq;
        std::iota(seq.begin(), seq.begin() + drawThreadCount, 0);
        std::for_each_n(
    // #if defined(__WINDOWS__)
    //         std::execution::par,
    // #endif
            seq.begin(), drawThreadCount,
            [this, &pipeline, &renderPassInfo, &frameIndex, &drawAtomic, &drawCount](uint32_t threadIt)
            {
                ZoneScoped;

                auto drawIt = drawAtomic++;
                if (drawIt >= drawCount)
                    return;

                static constexpr std::string_view drawPartitionStr = "WindowContext::drawPartition";
                char drawPartitionWithNumberStr[drawPartitionStr.size()+3];
                stbsp_sprintf(drawPartitionWithNumberStr, "%.*s%u",
                    static_cast<int>(drawPartitionStr.size()), drawPartitionStr.data(), threadIt);

                ZoneName(drawPartitionWithNumberStr, sizeof_array(drawPartitionWithNumberStr));
                
                CommandBufferInheritanceInfo<Vk> inheritInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
                inheritInfo.renderPass = renderPassInfo.renderPass;
                inheritInfo.framebuffer = renderPassInfo.framebuffer;

                CommandBufferAccessScopeDesc<Vk> beginInfo = {};
                beginInfo.flags =
                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                    VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
                beginInfo.pInheritanceInfo = &inheritInfo;
                beginInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                    
                auto& commandContext = myCommands[frameIndex][threadIt];
                auto cmd = commandContext->commands(beginInfo);

                // bind pipeline and inputs
                {
                    ZoneScopedN("WindowContext::drawViews::bind");

                    // bind pipeline and vertex/index buffers
                    pipeline->bindPipeline(cmd);

                    VkBuffer vertexBuffers[] = { *pipeline->resources().model };
                    VkDeviceSize vertexOffsets[] = { pipeline->resources().model->getVertexOffset() };

                    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
                    vkCmdBindIndexBuffer(cmd, *pipeline->resources().model,
                        pipeline->resources().model->getIndexOffset(), VK_INDEX_TYPE_UINT32);
                }

                uint32_t dx = renderPassInfo.renderArea.extent.width / myDesc.splitScreenGrid.width;
                uint32_t dy = renderPassInfo.renderArea.extent.height / myDesc.splitScreenGrid.height;

                while (drawIt < drawCount)
                {
                    auto drawView = [this, &drawCount, &frameIndex, &pipeline, &cmd, &dx, &dy](uint32_t viewIt)
                    {
                        uint32_t i = viewIt % myDesc.splitScreenGrid.width;
                        uint32_t j = viewIt / myDesc.splitScreenGrid.width;

                        uint32_t viewBufferOffset = (frameIndex * drawCount + viewIt) * sizeof(WindowContext::ViewBufferData);

                        pipeline->bindDescriptorSet(
                            cmd,
                            static_cast<uint32_t>(DescriptorSetCategory::Global),
                            std::make_optional(viewBufferOffset));

                        auto setViewportAndScissor = [](VkCommandBuffer cmd, int32_t x, int32_t y, int32_t width, int32_t height)
                        {
                            ZoneScopedN("WindowContext::drawViews::set");

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

                        setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

                        pipeline->bindDescriptorSet(cmd, static_cast<uint32_t>(DescriptorSetCategory::Material));

                        auto drawModel = [&pipeline](VkCommandBuffer cmd)
                        {
                            ZoneScopedN("WindowContext::drawViews::draw");

                            pipeline->bindDescriptorSet(cmd, static_cast<uint32_t>(DescriptorSetCategory::Object));

                            vkCmdDrawIndexed(cmd, pipeline->resources().model->getDesc().indexCount, 1, 0, 0, 0);
                        };

                        drawModel(cmd);
                    };

                    drawView(drawIt);

                    drawIt = drawAtomic++;
                }

                cmd.end();
            });
    }

    return drawThreadCount;
}

template <>
void WindowContext<Vk>::draw(const std::shared_ptr<PipelineContext<Vk>>& pipeline)
{
    ZoneScopedN("WindowContext::draw");

    auto frameIndex = mySwapchain->getFrameIndex();

    std::future<void> updateViewBufferFuture(std::async(std::launch::async, [this, frameIndex]
    {
        updateViewBuffer(frameIndex);
    }));

    auto& frame = *mySwapchain->getFrames()[frameIndex];
    auto& commandContext = myCommands[frameIndex][0];
    auto renderTarget = pipeline->getRenderTarget();

    auto cmd = commandContext->commands();
    auto renderPassInfo = renderTarget->begin(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    
    uint32_t drawThreadCount = internalDrawViews(pipeline, renderPassInfo.value(), frameIndex);
    
    for (uint32_t contextIt = 0; contextIt < drawThreadCount; contextIt++)
        commandContext->execute(*myCommands[frameIndex][contextIt]);

    //renderTarget->nextSubpass(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    renderTarget->end(cmd);

    frame.blit(cmd, renderTarget, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, 0);

    {
        ZoneScopedN("WindowContext::waitViewBuffer");

        updateViewBufferFuture.get();
    }
}

template <>
void WindowContext<Vk>::updateInput(const InputState& input)
{
    ZoneScopedN("WindowContext::updateInput");

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
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    WindowCreateDesc<Vk>&& desc)
: myDevice(deviceContext)
, myDesc(std::move(desc))
{
    ZoneScopedN("Window()");

    createFrameObjects(desc.framebufferExtent);

    const auto& graphicsCommandPools = myDevice->getQueueFamilies(myDevice->getGraphicsQueueFamilyIndex()).commandPools;

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
                myDevice,
                CommandContextCreateDesc<Vk>{graphicsCommandPools[frameIt * commandContextPerFrameCount + poolIt]}));
        }
    }

    myTimestamps[0] = std::chrono::high_resolution_clock::now();
}

template <>
WindowContext<Vk>::~WindowContext()
{
    ZoneScopedN("~Window()");
}
