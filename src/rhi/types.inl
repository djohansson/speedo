// the build's backends' types (their BackendTypes, see types.h): Vulkan's, rhi::kGraphicsApi's; and on macOS Metal's
// beside it while that backend grows (docs/backends-plan.md), which rhi's classes instantiate for its own tests
#include "vulkan/types.inl"
#if defined(__OSX__)
#include "metal/types.inl"
#endif
