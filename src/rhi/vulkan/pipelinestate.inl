namespace rhi
{

// the Vulkan backend's graphics state (see PipelineGraphicsState): the create infos vkCreateGraphicsPipelines takes, their
// pointers aimed at the vectors here (a snapshot of it aims them at its own copy, see Pipeline::GraphicsSnapshot)
template <>
struct PipelineGraphicsState<kVk>
{
	std::vector<PipelineShaderStageCreateInfo<kVk>> shaderStages;
	uint32_t shaderStageFlags{};
	PipelineVertexInputStateCreateInfo<kVk> vertexInput{};
	PipelineInputAssemblyStateCreateInfo<kVk> inputAssembly{};
	std::vector<NativeViewport<kVk>> viewports;
	std::vector<NativeRect<kVk>> scissorRects;
	PipelineViewportStateCreateInfo<kVk> viewport{};
	PipelineRasterizationStateCreateInfo<kVk> rasterization{};
	PipelineMultisampleStateCreateInfo<kVk> multisample{};
	PipelineDepthStencilStateCreateInfo<kVk> depthStencil{};
	std::vector<PipelineColorBlendAttachmentState<kVk>> colorBlendAttachments{};
	PipelineColorBlendStateCreateInfo<kVk> colorBlend{};
	std::vector<DynamicState<kVk>> dynamicStateDescs;
	PipelineDynamicStateCreateInfo<kVk> dynamicState{};
	std::optional<PipelineRenderingCreateInfo<kVk>> dynamicRendering{};
};

} // namespace rhi
