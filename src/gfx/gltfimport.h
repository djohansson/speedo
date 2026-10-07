#pragma once

#include <gfx/meshimport.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gfx::gltf
{

// imports a glTF 2.0 file (.gltf with external or embedded buffers, or .glb) and its materials, with cgltf (see
// CLAUDE.md's glTF notes for what is and isn't supported). a scene (options.scene, else the default one) is flattened
// into one mesh with the node transforms applied, except what animations move, instancing and skins, which keep their
// vertices in their own space and are drawn through Mesh::instances and Mesh::animation. images in buffers or data uris
// are named by the file and their index (see TextureRef::embeddedImage), and read with EmbeddedImage. returns an error
// message if the file can't be read or parsed, requires an extension that isn't supported, or if cancelled() returns
// true.
[[nodiscard]] std::expected<mesh::Mesh, std::string> Import(
	const std::filesystem::path& path, const mesh::ImportOptions& options = {}, const std::function<bool()>& cancelled = {});

// the first extension a gltf file requires that Import doesn't support (e.g. draco compression), if any. only parses
// the json, so it is cheap.
[[nodiscard]] std::optional<std::string> UnsupportedRequiredExtension(const std::filesystem::path& path);

// the bytes of an image a gltf file embeds (in a buffer, or as a data uri) by its index (see TextureRef::embeddedImage),
// or why they can't be read
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> EmbeddedImage(const std::filesystem::path& path, uint32_t index);

// the external buffer files a gltf file names (not images, which are loaded separately)
[[nodiscard]] std::vector<std::filesystem::path> BufferFiles(const std::filesystem::path& path);

} // namespace gfx::gltf
