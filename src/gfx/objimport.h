#pragma once

#include <gfx/meshimport.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gfx::obj
{

// imports a Wavefront OBJ file and its materials. polygons are triangulated, vertices are welded where they share all
// attributes, parts wound clockwise relative to their normals are flipped, missing normals are computed, texcoords are flipped to put v = 0 at the top of the image (the first row
// of an image in memory), and each vertex color is the vertex color in the file (if any) times its material's diffuse
// color and dissolve. returns an error message if the file can't be read or parsed, or if cancelled() returns true.
[[nodiscard]] std::expected<mesh::Mesh, std::string> Import(
	const std::filesystem::path& path, const std::function<bool()>& cancelled = {});

// the material files an import of path may read: all .mtl files next to it (tinyobjloader doesn't report which ones
// the obj file names, so this errs on the side of too many)
[[nodiscard]] std::vector<std::filesystem::path> MaterialFiles(const std::filesystem::path& path);

} // namespace gfx::obj
