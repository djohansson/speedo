// the build's backends' types (their BackendTypes, see types.h): Vulkan's, and on macOS Metal's beside it (a copy of gfx
// and the client is compiled for each, see gfx/graphicsapi.h)
#include "vulkan/types.inl"
#if defined(__OSX__)
#include "metal/types.inl"
#endif
