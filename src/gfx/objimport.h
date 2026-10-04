#pragma once

#include <gfx/bounds.h>
#include <gfx/shaders/capi.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gfx::obj
{

struct Material
{
	std::string name;
	std::array<float, 3> diffuse{1.0F, 1.0F, 1.0F}; // Kd
	float dissolve = 1.0F; // d (or 1 - Tr)
	// resolved paths of the textures the mtl file names, empty if it names none. a texture that can't be found is
	// counted in Stats::missingTextures and left empty.
	std::filesystem::path diffuseTexture; // map_Kd
	std::filesystem::path alphaTexture; // map_d
	std::filesystem::path normalTexture; // norm: a normal map
	std::filesystem::path bumpTexture; // map_bump, bump: a height map, or sometimes a normal map
	float bumpScale = 1.0F; // the bump texture's -bm option
};

// the indices of one material, contiguous in Mesh::indices
struct Submesh
{
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	int32_t material = -1; // index into Mesh::materials, or -1 for none
};

// what the import found in the file, and what it had to repair
struct Stats
{
	size_t triangleCount = 0;
	size_t degenerateTriangles = 0; // zero area, kept
	size_t droppedTriangles = 0; // referencing vertex data that doesn't exist
	size_t generatedNormals = 0; // vertices without a normal in the file, computed from the faces around them
	size_t repairedNormals = 0; // zero length or non-finite normals in the file, replaced by the face normal
	size_t nonFiniteValues = 0; // non-finite positions or texcoords, replaced by zero
	size_t missingTextures = 0; // textures named by a material that don't exist
	// parts (runs of faces with the same material in a shape) whose winding was reversed, since it was clockwise
	// relative to their normals on most of their area, as when an object has been mirrored on export
	size_t flippedParts = 0;
	// of the triangles with normals from the file, the area weighted fraction whose counter-clockwise winding agrees
	// with them (dot(face normal, vertex normals) > 0), after flipping parts. below 1 means some triangles disagree
	// with their normals, which can be fine (e.g. for two sided surfaces). negative if no triangle has normals from the file.
	double windingAgreement = -1.0;
	std::vector<std::string> warnings; // from the parser, and about textures that couldn't be found
};

struct Mesh
{
	std::vector<VertexP3fN3fT014fC4f> vertices;
	std::vector<uint32_t> indices; // triangle list
	std::vector<Submesh> submeshes; // ordered by material
	std::vector<Material> materials;
	Bounds3f bounds; // of the vertices
	bool hasNormals = false; // in the file, for at least one vertex. missing ones are generated (see Stats)
	bool hasTexCoords = false; // in the file, for at least one vertex. missing ones are zero
	bool hasColors = false; // in the file. vertex colors are 1 otherwise
	Stats stats;
};

// imports a Wavefront OBJ file and its materials. polygons are triangulated, vertices are welded where they share all
// attributes, parts wound clockwise relative to their normals are flipped, missing normals are computed, texcoords are flipped to put v = 0 at the top of the image (the first row
// of an image in memory), and each vertex color is the vertex color in the file (if any) times its material's diffuse
// color and dissolve. returns an error message if the file can't be read or parsed, or if cancelled() returns true.
[[nodiscard]] std::expected<Mesh, std::string> Import(
	const std::filesystem::path& path, const std::function<bool()>& cancelled = {});

// the material files an import of path may read: all .mtl files next to it (tinyobjloader doesn't report which ones
// the obj file names, so this errs on the side of too many)
[[nodiscard]] std::vector<std::filesystem::path> MaterialFiles(const std::filesystem::path& path);

} // namespace gfx::obj
