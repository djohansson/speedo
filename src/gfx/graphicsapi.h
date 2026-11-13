#pragma once

#include <rhi/capi.h>
#include <rhi/shaderset.h>

// the graphics api a copy of gfx (and of the client above it) is compiled for: gfx and the client are compiled once per
// graphics api the platform has (Vulkan, and Metal on macOS: CMakeLists.txt's speedo_graphics_api_libraries), each copy
// in an inline namespace of its own, SPEEDO_GRAPHICS_API_NAMESPACE (gfx::vk, gfx::metal, client::vk, ...), so that both
// link into one executable, which runs the one SPEEDO_GRAPHICS_API names (client/capi.cpp). rhi has every backend
#if !defined(SPEEDO_GRAPHICS_API_NAMESPACE)
#error "gfx is compiled per graphics api: SPEEDO_GRAPHICS_API_NAMESPACE names the copy (see CMakeLists.txt)"
#endif

namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

// the copy's graphics api, and the shader binaries its backend takes
#if defined(SPEEDO_GRAPHICS_API_METAL)
inline constexpr GraphicsApi kGraphicsApi = kMetal;
inline constexpr rhi::ShaderFormat kShaderFormat = rhi::ShaderFormat::kMsl;
#else
inline constexpr GraphicsApi kGraphicsApi = kVk;
inline constexpr rhi::ShaderFormat kShaderFormat = rhi::ShaderFormat::kSpirv16;
#endif

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
