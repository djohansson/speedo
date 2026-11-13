#include <rhi/vulkan/convert.h>

namespace rhi
{

namespace pipeline
{

// the Vulkan backend's part of Pipeline::SetDescriptorData (rhi/pipeline.inl): a neutral binding stored
// as vulkan's struct, and those structs compared per field (they have padding). comparing skips redundant descriptor
// updates: every update marks the descriptor set dirty, and binding a dirty set consumes a fresh descriptor set from the
// pool

[[nodiscard]] inline bool SameBindingValue(const DescriptorBufferInfo<kVk>& lhs, const DescriptorBufferInfo<kVk>& rhs) noexcept
{
	return lhs.buffer == rhs.buffer && lhs.offset == rhs.offset && lhs.range == rhs.range;
}

[[nodiscard]] inline bool SameBindingValue(const DescriptorImageInfo<kVk>& lhs, const DescriptorImageInfo<kVk>& rhs) noexcept
{
	return lhs.sampler == rhs.sampler && lhs.imageView == rhs.imageView && lhs.imageLayout == rhs.imageLayout;
}

[[nodiscard]] inline DescriptorBufferInfo<kVk> ToBackendBinding(const BufferBinding<kVk>& binding) noexcept
{
	return {.buffer = binding.buffer, .offset = binding.offset, .range = binding.range};
}

[[nodiscard]] inline DescriptorImageInfo<kVk> ToBackendBinding(const ImageBinding<kVk>& binding) noexcept
{
	return {.sampler = binding.sampler, .imageView = binding.imageView, .imageLayout = vk::ToVk(binding.layout)};
}

} // namespace pipeline

} // namespace rhi
