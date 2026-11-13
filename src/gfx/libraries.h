#pragma once

#include <cgltf.h>

#include <string>
#include <vector>

// what gfx's sources take of the header only libraries beyond their headers: code over a library's implementation's
// internals, compiled with the implementations (gfx/libraries.cpp, once: not a copy of gfx's, see gfx/graphicsapi.h)
namespace gfx::libraries
{

// KHR_animation_pointer: each animation's channels' pointers (empty where a channel has none), read from the json with
// cgltf's jsmn (its implementation's), since cgltf skips a channel target's extensions. JSON pointer escapes (~1, ~0)
// are undone per segment by gfx/gltfimport.cpp's ParsePointer
[[nodiscard]] std::vector<std::vector<std::string>> AnimationPointers(const cgltf_data& data);

} // namespace gfx::libraries
