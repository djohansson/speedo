namespace rhi
{

namespace pipeline
{

// the Metal backend's part of Pipeline::SetDescriptorData (rhi/pipeline.inl): a neutral binding stored as the backend's
// struct (an image's layout dropped: Metal has none). its structs compare by their defaulted ==

[[nodiscard]] inline DescriptorBufferInfo<kMetal> ToBackendBinding(const BufferBinding<kMetal>& binding) noexcept
{
	return {.buffer = binding.buffer, .offset = binding.offset, .range = binding.range};
}

[[nodiscard]] inline DescriptorImageInfo<kMetal> ToBackendBinding(const ImageBinding<kMetal>& binding) noexcept
{
	return {.sampler = binding.sampler, .imageView = binding.imageView};
}

} // namespace pipeline

} // namespace rhi
