namespace rhi
{

// the D3D12 backend's graphics state (see PipelineGraphicsState): the layout's graphics entry points, and the render
// target's attachments' formats and extent (SetRenderTarget), which a graphics pipeline state is made for; the fixed
// function state is the variant's (its blend modes, depth test, topology) and the command list's (viewport, scissor,
// stencil reference; the cull mode is a pipeline state's in D3D12, see the backend's pipeline.cpp)
template <>
struct PipelineGraphicsState<kD3D12>
{
	std::vector<PipelineShaderStageCreateInfo<kD3D12>> shaderStages;
	std::optional<PipelineRenderingCreateInfo<kD3D12>> rendering;
	Extent2d extent{};
};

} // namespace rhi
