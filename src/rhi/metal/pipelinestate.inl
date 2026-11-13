namespace rhi
{

// the Metal backend's graphics state (see PipelineGraphicsState): the layout's graphics entry points, and the render
// target's attachments' formats and extent (SetRenderTarget), which a render pipeline state is made for; the fixed
// function state is the variant's (its blend modes, depth test, topology) and the encoder's (cull mode, viewport)
template <>
struct PipelineGraphicsState<kMetal>
{
	std::vector<PipelineShaderStageCreateInfo<kMetal>> shaderStages;
	std::optional<PipelineRenderingCreateInfo<kMetal>> rendering;
	Extent2d extent{};
};

} // namespace rhi
