#pragma once

#include <gfx/bounds.h>
#include <gfx/textureref.h>
#include <gfx/shaders/capi.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// what the model importers (obj::Import, gltf::Import) produce: a triangle mesh in the renderer's conventions
// (counter-clockwise front faces, y up, v = 0 at the top of the image), with its materials baked into the vertex colors
namespace gfx::mesh
{

struct Material
{
	std::string name;
	// the color, already multiplied into the vertex colors: obj Kd and d (or 1 - Tr), gltf baseColorFactor
	std::array<float, 3> diffuse{1.0F, 1.0F, 1.0F};
	float dissolve = 1.0F;
	// the textures the material names (see TextureRef), empty if it names none
	TextureRef diffuseTexture; // obj map_Kd, gltf baseColorTexture
	TextureRef alphaTexture; // obj map_d
	TextureRef normalTexture; // obj norm, gltf normalTexture: a normal map
	float normalScale = 1.0F; // gltf normalTexture.scale: scales the normal map's x and y (0 flattens it)
	// light the surface gives off, added after lighting: obj Ke, gltf emissiveFactor (times KHR_materials_emissive_strength),
	// times the emissive texture's color if there is one. linear, and may be above 1.
	std::array<float, 3> emissive{0.0F, 0.0F, 0.0F};
	TextureRef emissiveTexture; // obj map_Ke, gltf emissiveTexture
	// gltf occlusionTexture: ambient occlusion in its red channel, which darkens the ambient (indirect) light, by strength
	TextureRef occlusionTexture;
	float occlusionStrength = 1.0F;
	TextureRef bumpTexture; // obj map_bump, bump: a height map, or sometimes a normal map
	float bumpScale = 1.0F; // the bump texture's -bm option
	// fragments whose diffuse texture alpha (or alpha texture value) is below this are discarded: 0 for opaque
	// materials (gltf OPAQUE), which ignore the alpha. obj materials are all alpha tested.
	float alphaCutoff = 0.5F;
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
	// obj: parts (runs of faces with the same material in a shape) whose winding was reversed, since it was clockwise
	// relative to their normals on most of their area, as when an object has been mirrored on export
	size_t flippedParts = 0;
	// of the triangles with normals from the file, the area weighted fraction whose counter-clockwise winding agrees
	// with them (dot(face normal, vertex normals) > 0), after flipping parts. below 1 means some triangles disagree
	// with their normals, which can be fine (e.g. for two sided surfaces). negative if no triangle has normals from the file.
	double windingAgreement = -1.0;
	std::vector<std::string> warnings; // from the parser, about textures that couldn't be found, and what was skipped
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

struct ImportOptions
{
	// where to write the images a gltf file embeds (in a buffer or as a data uri), which are loaded from files like the
	// others. empty: embedded images are counted as missing textures.
	std::filesystem::path embeddedImageDirectory;
};

// whether path is a model file Import takes, by its extension (.obj, .gltf, .glb)
[[nodiscard]] bool IsModelFile(const std::filesystem::path& path);

// imports a model file with the importer for its extension. returns an error message if the file can't be read or
// parsed, or if cancelled() returns true.
[[nodiscard]] std::expected<Mesh, std::string> Import(
	const std::filesystem::path& path, const ImportOptions& options = {}, const std::function<bool()>& cancelled = {});

// why Import can't load path at all, before trying (a gltf file requiring an unsupported extension), if it can't
[[nodiscard]] std::optional<std::string> Unsupported(const std::filesystem::path& path);

// the other files an import of path reads, which a change to must reimport it: an obj file's material files, a gltf
// file's external buffers (images are loaded separately)
[[nodiscard]] std::vector<std::filesystem::path> Dependencies(const std::filesystem::path& path);

} // namespace gfx::mesh
