// rhi's class templates instantiated for the Vulkan backend, as the Metal and D3D12 backends' are (their instantiate.cpp):
// every member defined in rhi's generic headers compiled with Vulkan's types (rhi/vulkan/types.inl), those no code calls
// with Vulkan's too, so that generic code naming another backend's types, or one only that backend has, fails every
// build here rather than when something first calls it. The backend's own members (declared in the headers, specialized
// in rhi/vulkan) aren't emitted; nothing references this file's code, so it adds no symbol a binary needs
#include <rhi/accelerationstructure.h>
#include <rhi/commandencoder.h>
#include <rhi/crashreport.h>
#include <rhi/frame.h>
#include <rhi/gpuprofiler.h>
#include <rhi/image.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>

template class rhi::AccelerationStructure<kVk>;
template class rhi::Buffer<kVk>;
template class rhi::BufferView<kVk>;
template class rhi::CommandBuffer<kVk>;
template class rhi::CommandEncoder<kVk>;
template class rhi::CommandPool<kVk>;
template class rhi::CrashBreadcrumb<kVk>;
template class rhi::DescriptorSetArray<kVk>;
template class rhi::DescriptorSetLayout<kVk>;
template class rhi::DescriptorUpdateTemplate<kVk>;
template class rhi::Device<kVk>;
template class rhi::Fence<kVk>;
template class rhi::Frame<kVk>;
template class rhi::GpuProfiler<kVk>;
template class rhi::Image<kVk>;
template class rhi::ImageView<kVk>;
template class rhi::Instance<kVk>;
template class rhi::MemoryBlock<kVk>;
template class rhi::Pipeline<kVk>;
template class rhi::PipelineLayout<kVk>;
template class rhi::QueryPool<kVk>;
template class rhi::RenderImageSet<kVk>;
template class rhi::Queue<kVk>;
template class rhi::SamplerVector<kVk>;
template class rhi::Semaphore<kVk>;
template class rhi::ShaderModule<kVk>;
template class rhi::Swapchain<kVk>;
