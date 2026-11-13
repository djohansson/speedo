// rhi's class templates instantiated for the Metal backend (docs/backends-plan.md): every member defined in rhi's
// generic headers compiled with Metal's types (rhi/metal/types.inl), so that generic code reaching into the Vulkan
// backend's structs, or naming its types, fails the macOS build here rather than when the Metal backend grows into it.
// The backend's own members (declared in the headers, specialized in rhi/metal) aren't emitted; nothing references
// this file's code, so it adds no symbol a binary needs. Frame waits for the backend's swapchain (step 6), whose frames
// it is
#include <rhi/accelerationstructure.h>
#include <rhi/commandencoder.h>
#include <rhi/crashreport.h>
#include <rhi/gpuprofiler.h>
#include <rhi/image.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>

template class rhi::AccelerationStructure<kMetal>;
template class rhi::Buffer<kMetal>;
template class rhi::BufferView<kMetal>;
template class rhi::CommandBuffer<kMetal>;
template class rhi::CommandEncoder<kMetal>;
template class rhi::CommandPool<kMetal>;
template class rhi::CrashBreadcrumb<kMetal>;
template class rhi::DescriptorSetArray<kMetal>;
template class rhi::DescriptorSetLayout<kMetal>;
template class rhi::DescriptorUpdateTemplate<kMetal>;
template class rhi::Device<kMetal>;
template class rhi::Fence<kMetal>;
template class rhi::GpuProfiler<kMetal>;
template class rhi::Image<kMetal>;
template class rhi::ImageView<kMetal>;
template class rhi::Instance<kMetal>;
template class rhi::MemoryBlock<kMetal>;
template class rhi::Pipeline<kMetal>;
template class rhi::PipelineLayout<kMetal>;
template class rhi::QueryPool<kMetal>;
template class rhi::RenderImageSet<kMetal>;
template class rhi::Queue<kMetal>;
template class rhi::SamplerVector<kMetal>;
template class rhi::Semaphore<kMetal>;
template class rhi::ShaderModule<kMetal>;
template class rhi::Swapchain<kMetal>;
