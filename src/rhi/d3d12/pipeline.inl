namespace rhi
{

namespace pipeline
{

// the D3D12 backend's part of Pipeline::SetDescriptorData (rhi/pipeline.inl): a neutral binding stored as the backend's
// struct (an image's layout dropped: a descriptor has none, the barriers set it). its structs compare by their defaulted ==

[[nodiscard]] inline DescriptorBufferInfo<kD3D12> ToBackendBinding(const BufferBinding<kD3D12>& binding) noexcept
{
	return {.buffer = binding.buffer, .offset = binding.offset, .range = binding.range};
}

[[nodiscard]] inline DescriptorImageInfo<kD3D12> ToBackendBinding(const ImageBinding<kD3D12>& binding) noexcept
{
	return {.sampler = binding.sampler, .imageView = binding.imageView};
}

} // namespace pipeline

} // namespace rhi
