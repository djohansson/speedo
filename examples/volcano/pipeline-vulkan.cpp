#include "pipeline.h"
#include "vk-utils.h"

template <>
void PipelineContext<GraphicsBackend::Vulkan>::createGraphicsPipeline()
{
    // todo: internal pipeline cache
    if (myConfig->graphicsPipeline)
        vkDestroyPipeline(getDeviceContext()->getDevice(), myConfig->graphicsPipeline, nullptr);

    VkPipelineShaderStageCreateInfo vsStageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    vsStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vsStageInfo.module = myConfig->layout->shaders[0];
    vsStageInfo.pName = "vertexMain"; // todo: get from named VkShaderModule object

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

    VkPipelineShaderStageCreateInfo fsStageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    fsStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fsStageInfo.module = myConfig->layout->shaders[1];
    fsStageInfo.pName = "fragmentMain"; // todo: get from named VkShaderModule object
    fsStageInfo.pSpecializationInfo = nullptr; //&specializationInfo;

    VkPipelineShaderStageCreateInfo shaderStages[] = {vsStageInfo, fsStageInfo};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInputInfo.vertexBindingDescriptionCount = myConfig->resources->model->getBindings().size();
    vertexInputInfo.pVertexBindingDescriptions = myConfig->resources->model->getBindings().data();
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(myConfig->resources->model->getDesc().attributes.size());
    vertexInputInfo.pVertexAttributeDescriptions = myConfig->resources->model->getDesc().attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    const auto& imageExtent = myConfig->resources->renderTarget->getRenderTargetDesc().imageExtent;

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(imageExtent.width);
    viewport.height = static_cast<float>(imageExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {imageExtent.width, imageExtent.height};

    VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
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

    VkPipelineMultisampleStateCreateInfo multisampling = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampling.minSampleShading = 1.0f;
    multisampling.pSampleMask = nullptr;
    multisampling.alphaToCoverageEnable = VK_FALSE;
    multisampling.alphaToOneEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
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

    VkPipelineColorBlendStateCreateInfo colorBlending = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
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
    VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
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
    pipelineInfo.layout = myConfig->layout->layout;
    pipelineInfo.renderPass = myConfig->resources->renderTarget->getRenderPass();
    pipelineInfo.subpass = myConfig->resources->renderTarget->getSubpass().value_or(0);
    pipelineInfo.basePipelineHandle = 0;
    pipelineInfo.basePipelineIndex = -1;

    //myPipelineMap.emplace()

    VK_CHECK(vkCreateGraphicsPipelines(
        getDeviceContext()->getDevice(),
        myCache,
        1,
        &pipelineInfo,
        nullptr,
        &myConfig->graphicsPipeline));
}


template <>
void PipelineContext<GraphicsBackend::Vulkan>::updateDescriptorSets(BufferHandle<GraphicsBackend::Vulkan> buffer) const
{
    ZoneScopedN("updateDescriptorSets");

    // todo: use reflection
    
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = myConfig->resources->image->getImageLayout();
    imageInfo.imageView = myConfig->resources->imageView->getImageViewHandle();
    imageInfo.sampler = myConfig->resources->sampler;

    std::array<VkWriteDescriptorSet, 3> descriptorWrites = {};
    descriptorWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[0].dstSet = myConfig->descriptorSets[0];
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfo;
    descriptorWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[1].dstSet = myConfig->descriptorSets[0];
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfo;
    descriptorWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[2].dstSet = myConfig->descriptorSets[0];
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(
        getDeviceContext()->getDevice(),
        static_cast<uint32_t>(descriptorWrites.size()),
        descriptorWrites.data(),
        0,
        nullptr);
}

template<>
PipelineContext<GraphicsBackend::Vulkan>::PipelineContext(
    const std::shared_ptr<InstanceContext<GraphicsBackend::Vulkan>>& instanceContext,
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const SerializableShaderReflectionModule<GraphicsBackend::Vulkan>& shaders,
    const std::filesystem::path& userProfilePath,
    PipelineContextCreateDesc<GraphicsBackend::Vulkan>&& desc)
: DeviceResource(deviceContext, desc)
, myDesc(std::move(desc))
, myInstance(instanceContext)
, myUserProfilePath(userProfilePath) // temp af
{
    myConfig = std::make_shared<PipelineConfiguration<GraphicsBackend::Vulkan>>();
    myConfig->resources = std::make_shared<PipelineResourceView<GraphicsBackend::Vulkan>>();
    myConfig->layout = std::make_shared<PipelineLayoutContext<GraphicsBackend::Vulkan>>(createPipelineLayoutContext(deviceContext->getDevice(), shaders));
    myConfig->resources->sampler = createSampler(deviceContext->getDevice());
    myConfig->descriptorSets = allocateDescriptorSets(
        deviceContext->getDevice(),
        deviceContext->getDescriptorPool(),
        myConfig->layout->descriptorSetLayouts.get(),
        myConfig->layout->descriptorSetLayouts.get_deleter().getSize());

    myCache = loadPipelineCache<GraphicsBackend::Vulkan>(
        std::filesystem::absolute(myUserProfilePath / "pipeline.cache"),
        deviceContext->getDevice(),
        instanceContext->getPhysicalDeviceInfos()[deviceContext->getDesc().physicalDeviceIndex].deviceProperties);
}

template<>
PipelineContext<GraphicsBackend::Vulkan>::~PipelineContext()
{
    savePipelineCache<GraphicsBackend::Vulkan>(
        myUserProfilePath / "pipeline.cache",
        getDeviceContext()->getDevice(),
        myInstance->getPhysicalDeviceInfos()[getDeviceContext()->getDesc().physicalDeviceIndex].deviceProperties,
        myCache);
    
    vkDestroyPipeline(getDeviceContext()->getDevice(), myConfig->graphicsPipeline, nullptr);

    vkDestroyPipelineCache(getDeviceContext()->getDevice(), myCache, nullptr);
    vkDestroyPipelineLayout(getDeviceContext()->getDevice(), myConfig->layout->layout, nullptr);
}