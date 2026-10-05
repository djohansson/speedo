#pragma once

#include <gfx/meshimport.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gfx::gltf
{

// imports a glTF 2.0 file (.gltf with external or embedded buffers, or .glb) and its materials, with cgltf. the default
// scene's meshes are flattened into one mesh: each node's transform is applied to its vertices (mirroring ones reverse
// the winding), and double sided materials get their triangles a second time, reversed, since the renderer culls back
// faces. missing normals are flat, as the spec says. texcoords keep their top left origin, texture transforms
// (KHR_texture_transform) are applied to them, and the texcoord set a material's textures use is put first. each
// vertex color is COLOR_0 (if any) times its material's base color factor, and alpha modes become an alpha cutoff
// (BLEND is drawn as MASK). images in buffers or data uris are written to options.embeddedImageDirectory. skins,
// animations, morph targets, cameras and lights are ignored. returns an error message if the file can't be read or
// parsed, requires an extension that isn't supported (e.g. draco or meshopt compression), or if cancelled() returns
// true.
[[nodiscard]] std::expected<mesh::Mesh, std::string> Import(
	const std::filesystem::path& path, const mesh::ImportOptions& options = {}, const std::function<bool()>& cancelled = {});

// the first extension a gltf file requires that Import doesn't support (e.g. draco compression), if any. only parses
// the json, so it is cheap.
[[nodiscard]] std::optional<std::string> UnsupportedRequiredExtension(const std::filesystem::path& path);

// the external buffer files a gltf file names (not images, which are loaded separately)
[[nodiscard]] std::vector<std::filesystem::path> BufferFiles(const std::filesystem::path& path);

} // namespace gfx::gltf
