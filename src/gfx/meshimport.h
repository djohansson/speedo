#pragma once

#include <gfx/bounds.h>
#include <gfx/scenecamera.h>
#include <gfx/scenelight.h>
#include <gfx/sceneanimation.h>
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
	// gltf metallic-roughness: the factors, times the texture's blue (metallic) and green (roughness) channels. obj
	// materials are dielectrics (metallic 0) as rough as their Ns says (Blinn-Phong to GGX: sqrt(2 / (Ns + 2)))
	float metallic = 0.0F;
	float roughness = 1.0F;
	TextureRef metallicRoughnessTexture;
	// the strength of the dielectric specular (the fresnel term, not metals'): gltf KHR_materials_specular's
	// specularFactor, times its specularTexture's alpha; obj Ks (its largest component: 0 is matte, without even a fresnel
	// rim)
	float specular = 1.0F;
	TextureRef specularTexture;
	// the dielectric specular's color at normal incidence is ((ior - 1) / (ior + 1))^2 (0.04 for the default 1.5,
	// KHR_materials_ior) times this color (KHR_materials_specular's specularColorFactor, times its srgb
	// specularColorTexture), at most 1, times the strength
	std::array<float, 3> specularColor{1.0F, 1.0F, 1.0F};
	TextureRef specularColorTexture;
	float ior = 1.5F;
	// KHR_materials_pbrSpecularGlossiness, for materials without metallic-roughness: a dielectric whose specular color at
	// normal incidence is specularColor itself, and specularColorTexture's rgb (srgb), whose diffuse is scaled by 1 minus
	// that color's largest component, and whose roughness is 1 - glossiness times the texture's alpha
	bool specularGlossiness = false;
	float glossiness = 1.0F;
	bool unlit = false; // gltf KHR_materials_unlit: drawn in its base color
	TextureRef bumpTexture; // obj map_bump, bump: a height map, or sometimes a normal map
	float bumpScale = 1.0F; // the bump texture's -bm option
	// fragments whose diffuse texture alpha (or alpha texture value) is below this are discarded: 0 for opaque
	// materials (gltf OPAQUE), which ignore the alpha. obj materials are all alpha tested.
	float alphaCutoff = 0.5F;
	// gltf doubleSided: its back faces are drawn too (not culled), lit as seen from behind. obj materials are single sided.
	bool doubleSided = false;
	// gltf BLEND: drawn blended by its alpha (base color times texture alpha), after the opaque ones, back to front by
	// submesh (see ModelSubmesh::center), without writing depth. alphaCutoff is 0.
	bool blend = false;
};

// the indices of one material and topology, contiguous in Mesh::indices
struct Submesh
{
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	int32_t material = -1; // index into Mesh::materials, or -1 for none
	// triangles, lines (pairs of indices) or points. lines and points without normals in the file keep zero normals,
	// which the shader draws unlit (gltf: base color plus emissive)
	rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::kTriangleList;
	// the instances it is drawn with (see Mesh::instances): one draw of instanceCount instances. the last
	// mirroredInstanceCount of them mirror (a negative determinant), which reverses the winding, so they are drawn
	// separately, with clockwise front faces
	uint32_t firstInstance = 0;
	uint32_t instanceCount = 1;
	uint32_t mirroredInstanceCount = 0;
	// its skin (an index into SceneAnimationData::skins), whose joints move its vertices (kept in the mesh's space,
	// with Mesh::skinVertices), or -1
	int32_t skin = -1;
	// its animated morph targets (see MorphDelta in gfx/shaders/capi.h, and SceneMorph), none if morphTargetCount is 0:
	// a row of morphTargetCount deltas per vertex in Mesh::morphDeltas from morphDeltaBase, for its vertices from
	// morphFirstVertex, weighted by the weights from morphWeightBase. the vertices are then unmorphed
	uint32_t morphTargetCount = 0;
	uint32_t morphDeltaBase = 0;
	uint32_t morphFirstVertex = 0;
	uint32_t morphWeightBase = 0;
};

// a column major 4x4 transform, from a submesh's vertices to world space
using Transform = std::array<float, 16>;
inline constexpr Transform kIdentityTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// what the import found in the file, and what it had to repair
struct Stats
{
	size_t triangleCount = 0;
	size_t lineCount = 0; // gltf lines, line strips and loops, as line segments
	size_t pointCount = 0;
	size_t degenerateTriangles = 0; // zero area, kept
	size_t droppedTriangles = 0; // referencing vertex data that doesn't exist (and lines and points that do)
	size_t generatedNormals = 0; // vertices without a normal in the file, computed from the faces around them
	size_t repairedNormals = 0; // zero length or non-finite normals in the file, replaced by the face normal
	size_t invalidTangents = 0; // zero length or non-finite tangents in the file (or w = 0), left for the shader to derive
	// gltf: vertices given MikkTSpace tangents, for normal mapped triangles without TANGENT (split where their corners'
	// tangents differ)
	size_t generatedTangents = 0;
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
	std::vector<VertexP3fN3fTa4fT014fC4f> vertices;
	std::vector<uint32_t> indices; // triangle list
	std::vector<Submesh> submeshes; // ordered by material
	// the instance transforms submeshes are drawn with. 0 is the identity, which all submeshes but the instanced ones
	// (gltf EXT_mesh_gpu_instancing) use: their vertices are in world space. instanced ones keep their vertices in their
	// node's space, and have a range of their own (the node's transform times each instance's).
	std::vector<Transform> instances{kIdentityTransform};
	// gltf: the file's scenes (their names, or "scene <index>"), and which one was loaded. empty for obj files, and gltf
	// files without scenes (whose root nodes are loaded)
	std::vector<std::string> scenes;
	uint32_t scene = 0;
	std::vector<SceneCamera> cameras; // gltf: the cameras of the scene's nodes, in the order they are visited
	std::vector<SceneLight> lights; // gltf KHR_lights_punctual: the lights of the scene's nodes
	// what moves: the nodes animations move or skins use, with the instances that follow them (see SceneAnimationData)
	SceneAnimationData animation;
	// parallel to vertices if any submesh is skinned (zero weights for the others), else empty
	std::vector<SkinVertex> skinVertices;
	// the animated morph targets' deltas (see Submesh::morphTargetCount), and every SceneMorph's default weights (the
	// node's, else its mesh's)
	std::vector<MorphDelta> morphDeltas;
	std::vector<float> morphWeights;
	std::vector<Material> materials;
	Bounds3f bounds; // of the vertices, in world space (each instanced submesh's at each of its instances)
	bool hasNormals = false; // in the file, for at least one vertex. missing ones are generated (see Stats)
	bool hasTangents = false; // in the file (gltf), for at least one vertex. without them, w = 0 (see the vertex's tangent)
	bool hasTexCoords = false; // in the file, for at least one vertex. missing ones are zero
	bool hasColors = false; // in the file. vertex colors are 1 otherwise
	Stats stats;
};

struct ImportOptions
{
	// gltf: the scene to load (an index into the file's scenes), else its default scene, or the first
	std::optional<size_t> scene;
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
