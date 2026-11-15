#include <rhi/vulkan/rendertarget.inl>
#if defined(__OSX__)
#include <rhi/metal/rendertarget.inl>
#endif
#if defined(__WINDOWS__)
#include <rhi/d3d12/rendertarget.inl>
#endif
