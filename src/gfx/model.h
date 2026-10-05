#pragma once

#include <gfx/bounds.h>
#include <gfx/gpu.h>
#include <gfx/scenecamera.h>
#include <gfx/scenelight.h>
#include <gfx/textureref.h>
#include <gfx/upload.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gfx
{

// the indices drawn with one material and topology, contiguous in the index buffer
struct ModelSubmesh
{
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	int32_t material = -1; // index into ModelDesc::materials, or -1 for none
	rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::kTriangleList; // see mesh::Submesh::topology
	// the center of its vertices' bounds, which blended submeshes are sorted by (back to front, per view)
	std::array<float, 3> center{};
	uint32_t firstInstance = 0; // see mesh::Submesh::firstInstance
	uint32_t instanceCount = 1;
	uint32_t mirroredInstanceCount = 0;
};

// textures are empty (see TextureRef) if the material has none
struct ModelMaterial
{
	std::string name;
	TextureRef diffuseTexture; // its colors are multiplied with the vertex colors
	TextureRef alphaTexture; // a mask, alpha tested
	TextureRef normalTexture; // a normal map, used rather than bumpTexture if there are both
	float normalScale = 1.0F; // see mesh::Material::normalScale
	std::array<float, 3> emissive{0.0F, 0.0F, 0.0F}; // see mesh::Material::emissive
	TextureRef emissiveTexture;
	TextureRef occlusionTexture; // see mesh::Material::occlusionTexture
	float occlusionStrength = 1.0F;
	float metallic = 0.0F; // see mesh::Material::metallic
	float roughness = 1.0F;
	TextureRef metallicRoughnessTexture;
	bool unlit = false;
	TextureRef bumpTexture; // a height map or a normal map
	float bumpScale = 1.0F; // for a bump texture that is a height map: see image::Options::bumpScale
	float alphaCutoff = 0.5F; // see mesh::Material::alphaCutoff
	bool doubleSided = false; // see mesh::Material::doubleSided
	bool blend = false; // see mesh::Material::blend
};

struct ModelDesc
{
	std::string name;
	Bounds3f bounds;
	uint32_t indexCount = 0; // uint32_t indices: triangle, line or point lists (see ModelSubmesh::topology)
	uint32_t vertexCount = 0; // VertexP3fN3fTa4fT014fC4f vertices (see gfx/shaders/capi.h)
	std::vector<ModelSubmesh> submeshes;
	std::vector<ModelMaterial> materials;
	// column major instance transforms (see mesh::Mesh::instances), in the model's instance buffer (gModelInstances)
	std::vector<std::array<float, 16>> instances;
	// the file's cameras (see mesh::Mesh::cameras). none for a set of several files (see Model::Load)
	std::vector<SceneCamera> cameras;
	// the file's lights (see mesh::Mesh::lights). none for a set of several files, which get the default light
	std::vector<SceneLight> lights;
};

// a mesh on the gpu: its index, vertex and instance buffers, drawn a submesh (material, topology and instances) at a time
class Model final
{
public:
	Model(ModelDesc&& desc, Buffer&& indexBuffer, Buffer&& vertexBuffer, Buffer&& instanceBuffer, const Upload& upload) noexcept;
	Model(const Model&) = delete;
	Model(Model&&) noexcept = delete;
	~Model();

	Model& operator=(const Model&) = delete;
	Model& operator=(Model&&) noexcept = delete;

	[[nodiscard]] const ModelDesc& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] const Buffer& GetIndexBuffer() const noexcept { return myIndexBuffer; }
	[[nodiscard]] const Buffer& GetVertexBuffer() const noexcept { return myVertexBuffer; }
	// ModelInstance (gfx/shaders/capi.h) per ModelDesc::instances: the transform and its inverse transpose
	[[nodiscard]] const Buffer& GetInstanceBuffer() const noexcept { return myInstanceBuffer; }
	// the upload of its buffers, which gpu work that uses them must wait for and acquire them from
	[[nodiscard]] const Upload& GetUpload() const noexcept { return myUpload; }

	// loads a model file (.obj, .gltf or .glb) through the asset cache (see core::file::LoadAsset and mesh::Import) and
	// uploads it on the
	// primary device's transfer queue. returns once the upload is submitted (see GetUpload), or null if the load was
	// cancelled because the application is exiting, or failed (the reason is printed to stderr).
	[[nodiscard]] static std::shared_ptr<Model> Load(std::string_view filePath, std::atomic_uint8_t& progress);
	// loads several model files (each through its own cache entry) as one, side by side in a grid in the xy plane, each
	// scaled to the same size and centered in its cell: for sets of variants, such as an archive's models, or the
	// encodings of a gltf model. files the importers don't support (see mesh::Unsupported) are skipped. null if any
	// load is cancelled or fails.
	[[nodiscard]] static std::shared_ptr<Model> Load(std::span<const std::string_view> filePaths, std::atomic_uint8_t& progress);

private:
	ModelDesc myDesc;
	Buffer myIndexBuffer;
	Buffer myVertexBuffer;
	Buffer myInstanceBuffer;
	Upload myUpload;
};

} // namespace gfx
