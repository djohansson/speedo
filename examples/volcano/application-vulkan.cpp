#include "application.h"

template <>
std::tuple<
    DeviceHandle<GraphicsBackend::Vulkan>, PhysicalDeviceHandle<GraphicsBackend::Vulkan>, PhysicalDeviceProperties<GraphicsBackend::Vulkan>, SwapchainInfo<GraphicsBackend::Vulkan>, SurfaceFormat<GraphicsBackend::Vulkan>, PresentMode<GraphicsBackend::Vulkan>, uint32_t,
    QueueHandle<GraphicsBackend::Vulkan>, int>
Application<GraphicsBackend::Vulkan>::createDevice(InstanceHandle<GraphicsBackend::Vulkan> instance, SurfaceHandle<GraphicsBackend::Vulkan> surface) const
{
    ZoneScoped;

    DeviceHandle<GraphicsBackend::Vulkan> logicalDevice;
    PhysicalDeviceHandle<GraphicsBackend::Vulkan> physicalDevice;
    SwapchainInfo<GraphicsBackend::Vulkan> swapChainInfo;
    SurfaceFormat<GraphicsBackend::Vulkan> selectedSurfaceFormat;
    PresentMode<GraphicsBackend::Vulkan> selectedPresentMode;
    uint32_t selectedFrameCount;
    QueueHandle<GraphicsBackend::Vulkan> selectedQueue;
    int selectedQueueFamilyIndex = -1;
    PhysicalDeviceProperties<GraphicsBackend::Vulkan> physicalDeviceProperties;

    uint32_t deviceCount = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
    if (deviceCount == 0)
        throw std::runtime_error("failed to find GPUs with Vulkan support!");

    std::vector<PhysicalDeviceHandle<GraphicsBackend::Vulkan>> devices(deviceCount);
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()));

    for (const auto& device : devices)
    {
        std::tie(swapChainInfo, selectedQueueFamilyIndex, physicalDeviceProperties) =
            getSuitableSwapchainAndQueueFamilyIndex<GraphicsBackend::Vulkan>(surface, device);

        if (selectedQueueFamilyIndex >= 0)
        {
            physicalDevice = device;

            const Format<GraphicsBackend::Vulkan> requestSurfaceImageFormat[] = {
                VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM,
                VK_FORMAT_R8G8B8_UNORM};
            const ColorSpace<GraphicsBackend::Vulkan> requestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            const PresentMode<GraphicsBackend::Vulkan> requestPresentMode[] = {
                VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR,
                VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};

            // Request several formats, the first found will be used
            // If none of the requested image formats could be found, use the first available
            selectedSurfaceFormat = swapChainInfo.formats[0];
            for (uint32_t request_i = 0; request_i < sizeof_array(requestSurfaceImageFormat);
                    request_i++)
            {
                SurfaceFormat<GraphicsBackend::Vulkan> requestedFormat = {requestSurfaceImageFormat[request_i],
                                                        requestSurfaceColorSpace};
                auto formatIt = std::find(
                    swapChainInfo.formats.begin(), swapChainInfo.formats.end(),
                    requestedFormat);
                if (formatIt != swapChainInfo.formats.end())
                {
                    selectedSurfaceFormat = *formatIt;
                    break;
                }
            }

            // Request a certain mode and confirm that it is available. If not use
            // VK_PRESENT_MODE_FIFO_KHR which is mandatory
            selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
            selectedFrameCount = 2;
            for (uint32_t request_i = 0; request_i < sizeof_array(requestPresentMode);
                    request_i++)
            {
                auto modeIt = std::find(
                    swapChainInfo.presentModes.begin(), swapChainInfo.presentModes.end(),
                    requestPresentMode[request_i]);
                if (modeIt != swapChainInfo.presentModes.end())
                {
                    selectedPresentMode = *modeIt;

                    if (selectedPresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
                        selectedFrameCount = 3;

                    break;
                }
            }

            break;
        }
    }

    if (!physicalDevice)
        throw std::runtime_error("failed to find a suitable GPU!");

    const float graphicsQueuePriority = 1.0f;

    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = selectedQueueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &graphicsQueuePriority;

    VkPhysicalDeviceFeatures deviceFeatures = {};
    deviceFeatures.samplerAnisotropy = VK_TRUE;

    uint32_t deviceExtensionCount;
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &deviceExtensionCount, nullptr);

    std::vector<VkExtensionProperties> availableDeviceExtensions(deviceExtensionCount);
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &deviceExtensionCount, availableDeviceExtensions.data());

    std::vector<const char*> deviceExtensions;
    deviceExtensions.reserve(deviceExtensionCount);
    for (uint32_t i = 0; i < deviceExtensionCount; i++)
    {
#if defined(__APPLE__)
        if (strcmp(availableDeviceExtensions[i].extensionName, "VK_MVK_moltenvk") == 0 ||
            strcmp(availableDeviceExtensions[i].extensionName, "VK_KHR_surface") == 0 ||
            strcmp(availableDeviceExtensions[i].extensionName, "VK_MVK_macos_surface") == 0)
            continue;
#endif
        deviceExtensions.push_back(availableDeviceExtensions[i].extensionName);
        std::cout << deviceExtensions.back() << "\n";
    }

    std::sort(
        deviceExtensions.begin(), deviceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

    std::vector<const char*> requiredDeviceExtensions = {
        // must be sorted lexicographically for std::includes to work!
        "VK_KHR_swapchain"};

    assert(std::includes(
        deviceExtensions.begin(), deviceExtensions.end(), requiredDeviceExtensions.begin(),
        requiredDeviceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
    deviceCreateInfo.enabledExtensionCount =
        static_cast<uint32_t>(requiredDeviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

    CHECK_VK(vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &logicalDevice));

    vkGetDeviceQueue(logicalDevice, selectedQueueFamilyIndex, 0, &selectedQueue);

    return std::make_tuple(
        logicalDevice, physicalDevice, physicalDeviceProperties, swapChainInfo, selectedSurfaceFormat,
        selectedPresentMode, selectedFrameCount, selectedQueue, selectedQueueFamilyIndex);
}

template <>
SwapchainContext<GraphicsBackend::Vulkan> Application<GraphicsBackend::Vulkan>::createSwapchainContext(
    DeviceHandle<GraphicsBackend::Vulkan> device,
    PhysicalDeviceHandle<GraphicsBackend::Vulkan> physicalDevice,
    VmaAllocator allocator,
    uint32_t frameCount,
    const WindowData<GraphicsBackend::Vulkan>& window) const
{
    ZoneScoped;

    SwapchainContext<GraphicsBackend::Vulkan> outSwapchain = {};
    {
        VkSwapchainCreateInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = window.surface;
        info.minImageCount = frameCount;
        info.imageFormat = window.surfaceFormat.format;
        info.imageColorSpace = window.surfaceFormat.colorSpace;
        info.imageExtent = {window.framebufferWidth, window.framebufferHeight};
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode =
            VK_SHARING_MODE_EXCLUSIVE; // Assume that graphics family == present family
        info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = window.presentMode;
        info.clipped = VK_TRUE;
        info.oldSwapchain = window.swapchain.swapchain;

        CHECK_VK(vkCreateSwapchainKHR(device, &info, nullptr, &outSwapchain.swapchain));
    }

    if (window.swapchain.swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device, window.swapchain.swapchain, nullptr);

    uint32_t imageCount;
    CHECK_VK(
        vkGetSwapchainImagesKHR(device, outSwapchain.swapchain, &imageCount, nullptr));
    
    outSwapchain.colorImages.resize(imageCount);
    outSwapchain.colorImageViews.resize(imageCount);
    
    CHECK_VK(vkGetSwapchainImagesKHR(
        device, outSwapchain.swapchain, &imageCount,
        outSwapchain.colorImages.data()));

    return outSwapchain;
}

template <>
AllocatorHandle<GraphicsBackend::Vulkan> Application<GraphicsBackend::Vulkan>::createAllocator(
    DeviceHandle<GraphicsBackend::Vulkan> device, PhysicalDeviceHandle<GraphicsBackend::Vulkan> physicalDevice) const
{
    ZoneScoped;

    auto vkGetBufferMemoryRequirements2KHR =
        (PFN_vkGetBufferMemoryRequirements2KHR)vkGetInstanceProcAddr(
            myInstance, "vkGetBufferMemoryRequirements2KHR");
    assert(vkGetBufferMemoryRequirements2KHR != nullptr);

    auto vkGetImageMemoryRequirements2KHR =
        (PFN_vkGetImageMemoryRequirements2KHR)vkGetInstanceProcAddr(
            myInstance, "vkGetImageMemoryRequirements2KHR");
    assert(vkGetImageMemoryRequirements2KHR != nullptr);

    VmaVulkanFunctions functions = {};
    functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
    functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    functions.vkAllocateMemory = vkAllocateMemory;
    functions.vkFreeMemory = vkFreeMemory;
    functions.vkMapMemory = vkMapMemory;
    functions.vkUnmapMemory = vkUnmapMemory;
    functions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
    functions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
    functions.vkBindBufferMemory = vkBindBufferMemory;
    functions.vkBindImageMemory = vkBindImageMemory;
    functions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
    functions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
    functions.vkCreateBuffer = vkCreateBuffer;
    functions.vkDestroyBuffer = vkDestroyBuffer;
    functions.vkCreateImage = vkCreateImage;
    functions.vkDestroyImage = vkDestroyImage;
    functions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2KHR;
    functions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2KHR;

    VmaAllocator allocator;
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;
    allocatorInfo.pVulkanFunctions = &functions;
    vmaCreateAllocator(&allocatorInfo, &allocator);

    return allocator;
}

template <>
DescriptorPoolHandle<GraphicsBackend::Vulkan> Application<GraphicsBackend::Vulkan>::createDescriptorPool() const
{
    ZoneScoped;

    constexpr uint32_t maxDescriptorCount = 1000;
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, maxDescriptorCount}};

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizeof_array(poolSizes));
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = maxDescriptorCount * static_cast<uint32_t>(sizeof_array(poolSizes));
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    DescriptorPoolHandle<GraphicsBackend::Vulkan> outDescriptorPool;
    CHECK_VK(vkCreateDescriptorPool(myDevice, &poolInfo, nullptr, &outDescriptorPool));

    return outDescriptorPool;
}

template <>
RenderPassHandle<GraphicsBackend::Vulkan> Application<GraphicsBackend::Vulkan>::createRenderPass(
    DeviceHandle<GraphicsBackend::Vulkan> device, const WindowData<GraphicsBackend::Vulkan>& window) const
{
    ZoneScoped;

    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = window.surfaceFormat.format;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = window.zBuffer->getDesc().format;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef = {};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef = {};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    RenderPassHandle<GraphicsBackend::Vulkan> outRenderPass;
    CHECK_VK(vkCreateRenderPass(device, &renderPassInfo, nullptr, &outRenderPass));

    return outRenderPass;
}

template <>
PipelineHandle<GraphicsBackend::Vulkan> Application<GraphicsBackend::Vulkan>::createGraphicsPipeline(
    DeviceHandle<GraphicsBackend::Vulkan> device,
    PipelineCacheHandle<GraphicsBackend::Vulkan> pipelineCache,
    const PipelineConfiguration<GraphicsBackend::Vulkan>& pipelineConfig) const
{
    ZoneScoped;

    VkPipelineShaderStageCreateInfo vsStageInfo = {};
    vsStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vsStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vsStageInfo.module = pipelineConfig.layout->shaders[0];
    vsStageInfo.pName = "main"; // todo: get from named VkShaderModule object

    // struct AlphaTestSpecializationData
    // {
    // 	uint32_t alphaTestMethod = 0;
    // 	float alphaTestRef = 0.5f;
    // } alphaTestSpecializationData;

    // std::array<VkSpecializationMapEntry, 2> alphaTestSpecializationMapEntries;
    // alphaTestSpecializationMapEntries[0].constantID = 0;
    // alphaTestSpecializationMapEntries[0].size =
    // sizeof(alphaTestSpecializationData.alphaTestMethod);
    // alphaTestSpecializationMapEntries[0].offset = 0;
    // alphaTestSpecializationMapEntries[1].constantID = 1;
    // alphaTestSpecializationMapEntries[1].size =
    // sizeof(alphaTestSpecializationData.alphaTestRef);
    // alphaTestSpecializationMapEntries[1].offset =
    // offsetof(AlphaTestSpecializationData, alphaTestRef);

    // VkSpecializationInfo specializationInfo = {};
    // specializationInfo.dataSize = sizeof(alphaTestSpecializationData);
    // specializationInfo.mapEntryCount =
    // static_cast<uint32_t>(alphaTestSpecializationMapEntries.size());
    // specializationInfo.pMapEntries = alphaTestSpecializationMapEntries.data();
    // specializationInfo.pData = &alphaTestSpecializationData;

    VkPipelineShaderStageCreateInfo fsStageInfo = {};
    fsStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fsStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fsStageInfo.module = pipelineConfig.layout->shaders[1];
    fsStageInfo.pName = "main";
    fsStageInfo.pSpecializationInfo = nullptr; //&specializationInfo;

    VkPipelineShaderStageCreateInfo shaderStages[] = {vsStageInfo, fsStageInfo};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = pipelineConfig.resources->model->getBindings().size();
    vertexInputInfo.pVertexBindingDescriptions = pipelineConfig.resources->model->getBindings().data();
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(pipelineConfig.resources->model->getDesc().attributes.size());
    vertexInputInfo.pVertexAttributeDescriptions = pipelineConfig.resources->model->getDesc().attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(pipelineConfig.resources->window->framebufferWidth);
    viewport.height = static_cast<float>(pipelineConfig.resources->window->framebufferHeight);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {
        static_cast<uint32_t>(pipelineConfig.resources->window->framebufferWidth),
        static_cast<uint32_t>(pipelineConfig.resources->window->framebufferHeight)};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.depthBiasConstantFactor = 0.0f;
    rasterizer.depthBiasClamp = 0.0f;
    rasterizer.depthBiasSlopeFactor = 0.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampling.minSampleShading = 1.0f;
    multisampling.pSampleMask = nullptr;
    multisampling.alphaToCoverageEnable = VK_FALSE;
    multisampling.alphaToOneEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;
    depthStencil.stencilTestEnable = VK_FALSE;
    depthStencil.front = {};
    depthStencil.back = {};

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    colorBlending.blendConstants[0] = 0.0f;
    colorBlending.blendConstants[1] = 0.0f;
    colorBlending.blendConstants[2] = 0.0f;
    colorBlending.blendConstants[3] = 0.0f;

    std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                    VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = sizeof_array(shaderStages);
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineConfig.layout->layout;
    pipelineInfo.renderPass = pipelineConfig.renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    PipelineHandle<GraphicsBackend::Vulkan> outPipeline = 0;
    CHECK_VK(vkCreateGraphicsPipelines(
        device, pipelineCache, 1, &pipelineInfo, nullptr, &outPipeline));

    return outPipeline;
}

template <>
void Application<GraphicsBackend::Vulkan>::initIMGUI(WindowData<GraphicsBackend::Vulkan>& window, float dpiScaleX, float dpiScaleY) const
{
    ZoneScoped;

    using namespace ImGui;

    IMGUI_CHECKVERSION();
    CreateContext();
    auto& io = GetIO();
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    io.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.PixelSnapH = false;

    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

    std::filesystem::path fontPath(myResourcePath);
    fontPath = std::filesystem::absolute(fontPath);
    fontPath /= "fonts";
    fontPath /= "foo";

    const char* fonts[] = {
        "Cousine-Regular.ttf", "DroidSans.ttf",  "Karla-Regular.ttf",
        "ProggyClean.ttf",	 "ProggyTiny.ttf", "Roboto-Medium.ttf",
    };

    ImFont* defaultFont = nullptr;
    for (auto font : fonts)
    {
        fontPath.replace_filename(font);
        defaultFont = io.Fonts->AddFontFromFileTTF(
            fontPath.u8string().c_str(), 16.0f,
            &config);
    }

    // Setup style
    StyleColorsClassic();
    io.FontDefault = defaultFont;

    // Setup Vulkan binding
    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance = myInstance;
    initInfo.PhysicalDevice = myPhysicalDevice;
    initInfo.Device = myDevice;
    initInfo.QueueFamily = myQueueFamilyIndex;
    initInfo.Queue = myQueue;
    initInfo.PipelineCache = myPipelineCache;
    initInfo.DescriptorPool = myDescriptorPool;
    initInfo.MinImageCount = window.swapchain.colorImages.size();
    initInfo.ImageCount = window.swapchain.colorImages.size();
    initInfo.Allocator = nullptr; // myAllocator;
    // initInfo.HostAllocationCallbacks = nullptr;
    initInfo.CheckVkResultFn = CHECK_VK;
    ImGui_ImplVulkan_Init(&initInfo, myGraphicsPipelineConfig->renderPass);

    // Upload Fonts
    {
        auto commandBuffer = beginSingleTimeCommands(myDevice, myTransferCommandPool);

        //TracyVkZone(myTracyContext, commandBuffer, "uploadFontTexture");

        ImGui_ImplVulkan_CreateFontsTexture(commandBuffer);
        endSingleTimeCommands(myDevice, myQueue, commandBuffer, myTransferCommandPool);
        ImGui_ImplVulkan_DestroyFontUploadObjects();
    }
}

template <>
void Application<GraphicsBackend::Vulkan>::updateDescriptorSets(
    const WindowData<GraphicsBackend::Vulkan>& window, const PipelineConfiguration<GraphicsBackend::Vulkan>& pipelineConfig) const
{
    // todo: use reflection
    
    ZoneScoped;

    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = window.viewBuffer->getBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = pipelineConfig.resources->textureView;
    imageInfo.sampler = pipelineConfig.resources->sampler;

    std::array<VkWriteDescriptorSet, 3> descriptorWrites = {};
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = pipelineConfig.descriptorSets[0];
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfo;
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = pipelineConfig.descriptorSets[0];
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfo;
    descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[2].dstSet = pipelineConfig.descriptorSets[0];
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(
        myDevice, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(),
        0, nullptr);
}

template <>
void Application<GraphicsBackend::Vulkan>::createFrameResources(WindowData<GraphicsBackend::Vulkan>& window)
{
    ZoneScoped;

    window.views.resize(NX * NY);
    for (auto& view : window.views)
    {
        if (!view.viewport.width)
            view.viewport.width = window.framebufferWidth / NX;

        if (!view.viewport.height)
            view.viewport.height = window.framebufferHeight / NY;

        updateViewMatrix(view);
        updateProjectionMatrix(view);
    }

    auto createSwapchainImageViews = [this, &window]() {
        ZoneScopedN("createSwapchainImageViews");

        for (uint32_t i = 0; i < window.swapchain.colorImages.size(); i++)
            window.swapchain.colorImageViews[i] = createImageView2D(
                myDevice, window.swapchain.colorImages[i], window.surfaceFormat.format,
                VK_IMAGE_ASPECT_COLOR_BIT);
    };

    createSwapchainImageViews();

    // for (all referenced resources/shaders)
    // {
        myGraphicsPipelineConfig = std::make_shared<PipelineConfiguration<GraphicsBackend::Vulkan>>();
        *myGraphicsPipelineConfig = createPipelineConfig(
            myDevice,
            createRenderPass(myDevice, window),
            myDescriptorPool,
            myPipelineCache,
            myGraphicsPipelineLayout,
            myDefaultResources);
    //}

    window.clearEnable = true;
    window.clearValue.color.float32[0] = 0.4f;
    window.clearValue.color.float32[1] = 0.4f;
    window.clearValue.color.float32[2] = 0.5f;
    window.clearValue.color.float32[3] = 1.0f;

    myFrameCommandPools.resize(myCommandBufferThreadCount);
    for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
    {
        myFrameCommandPools[threadIt] = createCommandPool(
            myDevice,
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            myQueueFamilyIndex);
    }

    auto createFramebuffer = [this, &window](uint32_t frameIndex)
    {
        std::array<VkImageView, 2> attachments = {window.swapchain.colorImageViews[frameIndex], window.zBufferView};
        VkFramebufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = myGraphicsPipelineConfig->renderPass;
        info.attachmentCount = attachments.size();
        info.pAttachments = attachments.data();
        info.width = window.framebufferWidth;
        info.height = window.framebufferHeight;
        info.layers = 1;

        VkFramebuffer outFramebuffer = VK_NULL_HANDLE;
        CHECK_VK(vkCreateFramebuffer(myDevice, &info, nullptr, &outFramebuffer));

        return outFramebuffer;
    };

    window.frames.resize(window.swapchain.colorImages.size());
    for (uint32_t frameIt = 0; frameIt < window.frames.size(); frameIt++)
    {
        auto& frame = window.frames[frameIt];

        frame.index = frameIt;
        frame.frameBuffer = createFramebuffer(frameIt);
        frame.commandBuffers.resize(myCommandBufferThreadCount);

        for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
        {
            VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cmdInfo.commandPool = myFrameCommandPools[threadIt];
            cmdInfo.level = threadIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            cmdInfo.commandBufferCount = 1;
            CHECK_VK(vkAllocateCommandBuffers(myDevice, &cmdInfo, &frame.commandBuffers[threadIt]));
        }

        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CHECK_VK(vkCreateFence(myDevice, &fenceInfo, nullptr, &frame.fence));

        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK_VK(vkCreateSemaphore(
            myDevice, &semaphoreInfo, nullptr, &frame.renderCompleteSemaphore));
        CHECK_VK(vkCreateSemaphore(
            myDevice, &semaphoreInfo, nullptr, &frame.newImageAcquiredSemaphore));

        frame.graphicsFrameTimestamp = std::chrono::high_resolution_clock::now();
    }

    updateDescriptorSets(window, *myGraphicsPipelineConfig);

    // vkAcquireNextImageKHR uses semaphore from last frame -> cant use index 0 for first frame
    window.frameIndex = window.frames.size() - 1;

    // todo: set up on transfer commandlist
    myTracyContext = TracyVkContext(myPhysicalDevice, myDevice, myQueue, window.frames[0].commandBuffers[0]);
}

template <>
void Application<GraphicsBackend::Vulkan>::cleanupFrameResources()
{
    ZoneScoped;

    auto& window = *myDefaultResources->window;

    for (auto& frame : window.frames)
    {
        vkDestroyFence(myDevice, frame.fence, nullptr);
        vkDestroySemaphore(myDevice, frame.renderCompleteSemaphore, nullptr);
        vkDestroySemaphore(myDevice, frame.newImageAcquiredSemaphore, nullptr);

        for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
            vkFreeCommandBuffers(myDevice, myFrameCommandPools[threadIt], 1, &frame.commandBuffers[threadIt]);

        vkDestroyFramebuffer(myDevice, frame.frameBuffer, nullptr);
    }

    for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
        vkDestroyCommandPool(myDevice, myFrameCommandPools[threadIt], nullptr);	

    for (VkImageView imageView : window.swapchain.colorImageViews)
        vkDestroyImageView(myDevice, imageView, nullptr);

    vkDestroyRenderPass(myDevice, myGraphicsPipelineConfig->renderPass, nullptr);

    vkDestroyPipeline(myDevice, myGraphicsPipelineConfig->graphicsPipeline, nullptr);

    TracyVkDestroy(myTracyContext);
}

template <>
void Application<GraphicsBackend::Vulkan>::updateViewBuffer(WindowData<GraphicsBackend::Vulkan>& window) const
{
    ZoneScoped;

    void* data;
    CHECK_VK(vmaMapMemory(myAllocator, window.viewBuffer->getBufferMemory(), &data));

    for (uint32_t n = 0; n < (NX * NY); n++)
    {
        View::BufferData& ubo = reinterpret_cast<View::BufferData*>(data)[window.frameIndex * (NX * NY) + n];

        ubo.viewProj = window.views[n].projection * glm::mat4(window.views[n].view);
    }

    vmaFlushAllocation(
        myAllocator,
        window.viewBuffer->getBufferMemory(),
        sizeof(View::BufferData) * window.frameIndex * (NX * NY),
        sizeof(View::BufferData) * (NX * NY));

    vmaUnmapMemory(myAllocator, window.viewBuffer->getBufferMemory());
}

template <>
void Application<GraphicsBackend::Vulkan>::checkFlipOrPresentResult(WindowData<GraphicsBackend::Vulkan>& window, Result<GraphicsBackend::Vulkan> result) const
{
    switch (result)
    {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        std::cout << "warning: flip/present returned VK_SUBOPTIMAL_KHR";
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        window.createFrameResourcesFlag = true;
        break;
    default:
        throw std::runtime_error("failed to flip swap chain image!");
    }
}

template <>
Application<GraphicsBackend::Vulkan>::Application(
		void* view, int width, int height, int framebufferWidth, int framebufferHeight,
		const char* resourcePath)
		: myResourcePath(resourcePath)
		, myCommandBufferThreadCount(2)
		, myRequestedCommandBufferThreadCount(2)
{
    ZoneScoped;

    assert(std::filesystem::is_directory(myResourcePath));

    myInstance = createInstance<GraphicsBackend::Vulkan>();
    myDebugCallback = createDebugCallback(myInstance);

    myDefaultResources = std::make_shared<GraphicsPipelineResourceView<GraphicsBackend::Vulkan>>();
    myDefaultResources->window = std::make_shared<WindowData<GraphicsBackend::Vulkan>>();
    auto& window = *myDefaultResources->window;

    window.width = width;
    window.height = height;
    window.framebufferWidth = framebufferWidth;
    window.framebufferHeight = framebufferHeight;
    window.surface = createSurface(myInstance, view);

    uint32_t frameCount = 0;

    std::tie(
        myDevice, myPhysicalDevice, myPhysicalDeviceProperties, window.swapchain.info, window.surfaceFormat,
        window.presentMode, frameCount, myQueue, myQueueFamilyIndex) =
        createDevice(myInstance, window.surface);

    myAllocator = createAllocator(myDevice, myPhysicalDevice);

    myTransferCommandPool = createCommandPool(
        myDevice,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        myQueueFamilyIndex);

    myDefaultResources->sampler = createTextureSampler(myDevice);
    myDescriptorPool = createDescriptorPool();

    auto slangShaders = loadSlangShaders<GraphicsBackend::Vulkan>(std::filesystem::absolute(myResourcePath / "shaders" / "shaders.slang"));

    myGraphicsPipelineLayout = std::make_shared<PipelineLayoutContext<GraphicsBackend::Vulkan>>();
    *myGraphicsPipelineLayout = createPipelineLayoutContext(myDevice, *slangShaders);
    
    myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
        myDevice, myTransferCommandPool, myQueue, myAllocator,
        std::filesystem::absolute(myResourcePath / "models" / "gallery.obj"));
    myDefaultResources->texture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
        myDevice, myTransferCommandPool, myQueue, myAllocator,
        std::filesystem::absolute(myResourcePath / "images" / "gallery.jpg"));
    myDefaultResources->textureView = myDefaultResources->texture->createView(VK_IMAGE_ASPECT_COLOR_BIT);

    myPipelineCache = loadPipelineCache<GraphicsBackend::Vulkan>(
        myDevice,
        myPhysicalDeviceProperties,
        std::filesystem::absolute(myResourcePath / ".cache" / "pipeline.cache"));

    myDefaultResources->window->swapchain = createSwapchainContext(
        myDevice, myPhysicalDevice, myAllocator, frameCount,
        *myDefaultResources->window);

    BufferCreateDesc<GraphicsBackend::Vulkan> bufferDesc =
    {
        frameCount * (NX * NY) * sizeof(View::BufferData),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_NULL_HANDLE,
        VK_NULL_HANDLE,
        "viewBuffer"
    };
    window.viewBuffer = std::make_shared<Buffer<GraphicsBackend::Vulkan>>(myDevice, myTransferCommandPool, myQueue, myAllocator, std::move(bufferDesc));

    // todo: append stencil bit for depthstencil composite formats
    TextureCreateDesc<GraphicsBackend::Vulkan> textureData = 
    {
        window.framebufferWidth,
        window.framebufferHeight,
        1,
        0,
        findSupportedFormat(
            myPhysicalDevice,
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_NULL_HANDLE,
        VK_NULL_HANDLE, 
        "zBuffer"
    };
    window.zBuffer = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
        myDevice, myTransferCommandPool, myQueue, myAllocator,
        std::move(textureData));
    window.zBufferView = window.zBuffer->createView(VK_IMAGE_ASPECT_DEPTH_BIT);

    createFrameResources(*myDefaultResources->window);

    float dpiScaleX = static_cast<float>(framebufferWidth) / width;
    float dpiScaleY = static_cast<float>(framebufferHeight) / height;

    initIMGUI(window, dpiScaleX, dpiScaleY);
}

template <>
Application<GraphicsBackend::Vulkan>::~Application()
{
    ZoneScoped;

    {
        ZoneScopedN("deviceWaitIdle");

        CHECK_VK(vkDeviceWaitIdle(myDevice));
    }

    std::filesystem::path cacheFilePath = std::filesystem::absolute(myResourcePath / ".cache");

    if (!std::filesystem::exists(cacheFilePath))
        std::filesystem::create_directory(cacheFilePath);

    savePipelineCache<GraphicsBackend::Vulkan>(myDevice, myPipelineCache, myPhysicalDeviceProperties, cacheFilePath / "pipeline.cache");

    cleanupFrameResources();

    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();

    auto& window = *myDefaultResources->window;

    vkDestroySwapchainKHR(myDevice, window.swapchain.swapchain, nullptr);

    {
        vkDestroyImageView(myDevice, window.zBufferView, nullptr);
        window.zBuffer.reset();
        window.viewBuffer.reset();
        myDefaultResources->model.reset();
        vkDestroyImageView(myDevice, myDefaultResources->textureView, nullptr);
        myDefaultResources->texture.reset();
    }

    vkDestroyPipelineCache(myDevice, myPipelineCache, nullptr);
    vkDestroyPipelineLayout(myDevice, myGraphicsPipelineLayout->layout, nullptr);

    // todo: wrap these in a deleter.
    myGraphicsPipelineLayout->shaders.reset();
    myGraphicsPipelineLayout->descriptorSetLayouts.reset();

    vkDestroySampler(myDevice, myDefaultResources->sampler, nullptr);

    vkDestroyDescriptorPool(myDevice, myDescriptorPool, nullptr);
    vkDestroyCommandPool(myDevice, myTransferCommandPool, nullptr);

    char* allocatorStatsJSON = nullptr;
    vmaBuildStatsString(myAllocator, &allocatorStatsJSON, true);
    std::cout << allocatorStatsJSON << std::endl;
    vmaFreeStatsString(myAllocator, allocatorStatsJSON);
    vmaDestroyAllocator(myAllocator);

    vkDestroyDevice(myDevice, nullptr);
    vkDestroySurfaceKHR(myInstance, window.surface, nullptr);

    auto vkDestroyDebugReportCallbackEXT =
        (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(
            myInstance, "vkDestroyDebugReportCallbackEXT");
    assert(vkDestroyDebugReportCallbackEXT != nullptr);
    vkDestroyDebugReportCallbackEXT(myInstance, myDebugCallback, nullptr);

    vkDestroyInstance(myInstance, nullptr);
}

template <>
void Application<GraphicsBackend::Vulkan>::submitFrame(WindowData<GraphicsBackend::Vulkan>& window)
{
    ZoneScoped;

    window.lastFrameIndex = window.frameIndex;
    auto& lastFrame = window.frames[window.lastFrameIndex];

    {
        ZoneScopedN("acquireNextImage");

        checkFlipOrPresentResult(window, vkAcquireNextImageKHR(
            myDevice, window.swapchain.swapchain, UINT64_MAX, lastFrame.newImageAcquiredSemaphore, VK_NULL_HANDLE,
            &window.frameIndex));
    }

    auto& frame = window.frames[window.frameIndex];
    
    // wait for frame to be completed before starting to use it
    {
        ZoneScopedN("waitForFrameFence");

        CHECK_VK(vkWaitForFences(myDevice, 1, &frame.fence, VK_TRUE, UINT64_MAX));
        CHECK_VK(vkResetFences(myDevice, 1, &frame.fence));

        frame.graphicsFrameTimestamp = std::chrono::high_resolution_clock::now();
        frame.graphicsDeltaTime = frame.graphicsFrameTimestamp - lastFrame.graphicsFrameTimestamp;
    }

    std::future<void> updateViewBufferFuture(std::async(std::launch::async, [this, &window]
    {
        updateViewBuffer(window);
    }));

    std::future<void> secondaryCommandsFuture(std::async(std::launch::async, [this, &window, &frame]
    {
        // setup draw parameters
        constexpr uint32_t drawCount = NX * NY;
        uint32_t segmentCount = std::max(myCommandBufferThreadCount - 1u, 1u);

        assert(myGraphicsPipelineConfig);
        auto& config = *myGraphicsPipelineConfig;
        assert(config.resources);

        // begin secondary command buffers
        for (uint32_t segmentIt = 0; segmentIt < segmentCount; segmentIt++)
        {
            ZoneScopedN("beginSecondaryCommands");

            VkCommandBuffer cmd = frame.commandBuffers[segmentIt + 1];

            VkCommandBufferInheritanceInfo inherit = {
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
            inherit.renderPass = config.renderPass;
            inherit.framebuffer = frame.frameBuffer;

            CHECK_VK(vkResetCommandBuffer(cmd, 0));
            VkCommandBufferBeginInfo secBeginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            secBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                                VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
                                VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
            secBeginInfo.pInheritanceInfo = &inherit;
            CHECK_VK(vkBeginCommandBuffer(cmd, &secBeginInfo));
        }

        // draw geometry using secondary command buffers
        {
            ZoneScopedN("draw");

            uint32_t segmentDrawCount = drawCount / segmentCount;
            if (drawCount % segmentCount)
                segmentDrawCount += 1;

            uint32_t dx = window.framebufferWidth / NX;
            uint32_t dy = window.framebufferHeight / NY;

            std::array<uint32_t, 128> seq;
            std::iota(seq.begin(), seq.begin() + segmentCount, 0);
            std::for_each_n(
// #if defined(__WINDOWS__)
// 				std::execution::par,
// #endif
                seq.begin(), segmentCount,
                [&config, &frame, &dx, &dy, &segmentDrawCount](uint32_t segmentIt) {
                    ZoneScopedN("drawSegment");

                    VkCommandBuffer cmd = frame.commandBuffers[segmentIt + 1];

                    //TracyVkZone(myTracyContext, cmd, "draw");

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
                        //TracyVkZone(myTracyContext, cmd, "drawModel");
                        
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

                        auto drawModel = [&n, &frame](
                                            VkCommandBuffer cmd, uint32_t indexCount,
                                            uint32_t descriptorSetCount,
                                            const VkDescriptorSet* descriptorSets,
                                            VkPipelineLayout pipelineLayout) {
                            uint32_t viewBufferOffset = (frame.index * drawCount + n) * sizeof(View::BufferData);
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

        // end secondary command buffers
        for (uint32_t segmentIt = 0; segmentIt < segmentCount; segmentIt++)
        {
            VkCommandBuffer cmd = frame.commandBuffers
                    [segmentIt + 1];
            {
                ZoneScopedN("endSecondaryCommands");

                CHECK_VK(vkEndCommandBuffer(cmd));
            }
        }
    }));

    if (window.imguiEnable)
    {
        ImGui_ImplVulkan_NewFrame();
        drawIMGUI(window);
    }
    
    // begin primary command buffer
    {
        ZoneScopedN("beginCommands");

        CHECK_VK(vkResetCommandBuffer(frame.commandBuffers[0], 0));
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK_VK(vkBeginCommandBuffer(frame.commandBuffers[0], &info));
    }

    // collect timing scopes
    {
        ZoneScopedN("tracyVkCollect");

        TracyVkZone(myTracyContext, frame.commandBuffers[0], "tracyVkCollect");

        TracyVkCollect(myTracyContext, frame.commandBuffers[0]);
    }

    std::array<ClearValue<GraphicsBackend::Vulkan>, 2> clearValues = {};
    clearValues[0] = window.clearValue;
    clearValues[1].depthStencil = {1.0f, 0};

    // call secondary command buffers
    {
        {
            ZoneScopedN("waitSecondaryCommands");

            secondaryCommandsFuture.get();
        }

        ZoneScopedN("executeCommands");

        TracyVkZone(myTracyContext, frame.commandBuffers[0], "executeCommands");

        VkRenderPassBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = myGraphicsPipelineConfig->renderPass;
        beginInfo.framebuffer = frame.frameBuffer;
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent = {static_cast<uint32_t>(window.framebufferWidth),
                                        static_cast<uint32_t>(window.framebufferHeight)};
        beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        beginInfo.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(
            frame.commandBuffers[0], &beginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

        vkCmdExecuteCommands(frame.commandBuffers[0], (myCommandBufferThreadCount - 1), &frame.commandBuffers[1]);

        vkCmdEndRenderPass(frame.commandBuffers[0]);
    }

    // Record Imgui Draw Data and draw funcs into primary command buffer
    if (window.imguiEnable)
    {
        ZoneScopedN("drawIMGUI");

        TracyVkZone(myTracyContext, frame.commandBuffers[0], "drawIMGUI");

        VkRenderPassBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = myGraphicsPipelineConfig->renderPass;
        beginInfo.framebuffer = frame.frameBuffer;
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent.width = window.framebufferWidth;
        beginInfo.renderArea.extent.height = window.framebufferHeight;
        beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        beginInfo.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(frame.commandBuffers[0], &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commandBuffers[0]);

        vkCmdEndRenderPass(frame.commandBuffers[0]);
    }

    // Submit primary command buffer
    {
        {
            ZoneScopedN("waitFramePrepare");

            updateViewBufferFuture.get();
            
        }

        ZoneScopedN("submitCommands");

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &lastFrame.newImageAcquiredSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffers[0];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &frame.renderCompleteSemaphore;

        CHECK_VK(vkEndCommandBuffer(frame.commandBuffers[0]));
        CHECK_VK(vkQueueSubmit(myQueue, 1, &submitInfo, frame.fence));
    }
}

template <>
void Application<GraphicsBackend::Vulkan>::presentFrame(WindowData<GraphicsBackend::Vulkan>& window) const
{
    ZoneScoped;

    auto& frame = window.frames[window.frameIndex];

    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &frame.renderCompleteSemaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &window.swapchain.swapchain;
    info.pImageIndices = &window.frameIndex;
    checkFlipOrPresentResult(window, vkQueuePresentKHR(myQueue, &info));
}

template <>
void Application<GraphicsBackend::Vulkan>::draw()
{
    FrameMark;
    ZoneScoped;

    auto& window = *myDefaultResources->window;

    // re-create frame resources if needed
    if (window.createFrameResourcesFlag)
    {
        ZoneScopedN("recreateFrameResources");

        {
            ZoneScopedN("queueWaitIdle");

            CHECK_VK(vkQueueWaitIdle(myQueue));
        }

        cleanupFrameResources();

        myCommandBufferThreadCount = std::min(static_cast<uint32_t>(myRequestedCommandBufferThreadCount), NX * NY);

        createFrameResources(window);
    }

    updateInput(window);
    submitFrame(window);
    presentFrame(window);
}

template <>
void Application<GraphicsBackend::Vulkan>::resizeFramebuffer(int width, int height)
{
    ZoneScoped;

    auto& window = *myDefaultResources->window;
    auto frameCount = window.swapchain.colorImages.size();

    {
        ZoneScopedN("queueWaitIdle");

        CHECK_VK(vkQueueWaitIdle(myQueue));
    }

    cleanupFrameResources();

    // hack to shut up validation layer createPipelineLayoutContext.
    // see https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/624
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        myPhysicalDevice, window.surface, &window.swapchain.info.capabilities);

    // window.width = width;
    // window.height = height;
    window.framebufferWidth = width;
    window.framebufferHeight = height;

    myDefaultResources->window->swapchain = createSwapchainContext(
        myDevice, myPhysicalDevice, myAllocator, frameCount,
        *myDefaultResources->window);

    // todo: append stencil bit for depthstencil composite formats
    TextureCreateDesc<GraphicsBackend::Vulkan> textureData = 
    {
        window.framebufferWidth,
        window.framebufferHeight,
        1,
        0,
        findSupportedFormat(
            myPhysicalDevice,
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_NULL_HANDLE,
        VK_NULL_HANDLE,
        "zBuffer"
    };
    window.zBuffer = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
        myDevice, myTransferCommandPool, myQueue, myAllocator,
        std::move(textureData));
    window.zBufferView = window.zBuffer->createView(VK_IMAGE_ASPECT_DEPTH_BIT);

    createFrameResources(*myDefaultResources->window);
}
