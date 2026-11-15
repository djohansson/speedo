// the build's backends' types (their BackendTypes, see types.h): Vulkan's, and on macOS Metal's beside it, on Windows
// D3D12's (a copy of gfx and the client is compiled for each, see gfx/graphicsapi.h)
#include "vulkan/types.inl"
#if defined(__OSX__)
#include "metal/types.inl"
#endif
#if defined(__WINDOWS__)
#include "d3d12/types.inl"
#endif
