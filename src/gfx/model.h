#pragma once

#include <gfx/bounds.h>
#include <gfx/gpu.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gfx
{

// the indices drawn with one material, contiguous in the index buffer
struct ModelSubmesh
{
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	int32_t material = -1; // index into ModelDesc::materials, or -1 for none
};

// texture paths are empty if the material has none
struct ModelMaterial
{
	std::string name;
	std::string diffuseTexture; // its colors are multiplied with the vertex colors
	std::string alphaTexture; // a mask, alpha tested
	std::string normalTexture; // a normal map, used rather than bumpTexture if there are both
	std::string bumpTexture; // a height map or a normal map
	float bumpScale = 1.0F; // for a bump texture that is a height map: see image::Options::bumpScale
};

struct ModelDesc
{
	std::string name;
	Bounds3f bounds;
	uint32_t indexCount = 0; // uint32_t indices, a triangle list
	uint32_t vertexCount = 0; // VertexP3fN3fT014fC4f vertices (see gfx/shaders/capi.h)
	std::vector<ModelSubmesh> submeshes;
	std::vector<ModelMaterial> materials;
};

// a mesh on the gpu: its index and vertex buffers, drawn a submesh (material) at a time
class Model final
{
public:
	Model(ModelDesc&& desc, Buffer&& indexBuffer, Buffer&& vertexBuffer) noexcept;
	Model(const Model&) = delete;
	Model(Model&&) noexcept = delete;
	~Model();

	Model& operator=(const Model&) = delete;
	Model& operator=(Model&&) noexcept = delete;

	[[nodiscard]] const ModelDesc& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] const Buffer& GetIndexBuffer() const noexcept { return myIndexBuffer; }
	[[nodiscard]] const Buffer& GetVertexBuffer() const noexcept { return myVertexBuffer; }

	// loads a model file through the asset cache (see core::file::LoadAsset and obj::Import) and uploads it on the
	// primary device. returns once the upload has completed, or null if the load was cancelled because the application
	// is exiting, or failed (the reason is printed to stderr).
	[[nodiscard]] static std::shared_ptr<Model> Load(std::string_view filePath, std::atomic_uint8_t& progress);

private:
	ModelDesc myDesc;
	Buffer myIndexBuffer;
	Buffer myVertexBuffer;
};

} // namespace gfx
