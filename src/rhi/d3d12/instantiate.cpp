// rhi's class templates instantiated for the D3D12 backend (docs/backends-plan.md): every member defined in rhi's generic
// headers compiled with D3D12's types (rhi/d3d12/types.inl), so that generic code reaching into another backend's structs,
// or naming its types, fails the Windows build here rather than when the D3D12 backend grows into it. The backend's own
// members (declared in the headers, specialized in rhi/d3d12) aren't emitted; nothing references this file's code, so it
// adds no symbol a binary needs
#include <rhi/accelerationstructure.h>
#include <rhi/commandencoder.h>
#include <rhi/crashreport.h>
#include <rhi/gpuprofiler.h>
#include <rhi/image.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>

template class rhi::AccelerationStructure<kD3D12>;
template class rhi::Buffer<kD3D12>;
template class rhi::BufferView<kD3D12>;
template class rhi::CommandBuffer<kD3D12>;
template class rhi::CommandEncoder<kD3D12>;
template class rhi::CommandPool<kD3D12>;
template class rhi::CrashBreadcrumb<kD3D12>;
template class rhi::DescriptorSetArray<kD3D12>;
template class rhi::DescriptorSetLayout<kD3D12>;
template class rhi::DescriptorUpdateTemplate<kD3D12>;
template class rhi::Device<kD3D12>;
template class rhi::Fence<kD3D12>;
template class rhi::GpuProfiler<kD3D12>;
template class rhi::Image<kD3D12>;
template class rhi::ImageView<kD3D12>;
template class rhi::Instance<kD3D12>;
template class rhi::MemoryBlock<kD3D12>;
template class rhi::Pipeline<kD3D12>;
template class rhi::PipelineLayout<kD3D12>;
template class rhi::QueryPool<kD3D12>;
template class rhi::RenderImageSet<kD3D12>;
template class rhi::Queue<kD3D12>;
template class rhi::SamplerVector<kD3D12>;
template class rhi::Semaphore<kD3D12>;
template class rhi::ShaderModule<kD3D12>;
template class rhi::Swapchain<kD3D12>;
