#pragma once

#include <gfx/bounds.h>
#include <gfx/gpu.h>
#include <gfx/scenecamera.h>
#include <gfx/scenelight.h>
#include <gfx/sceneanimation.h>
#include <gfx/textureref.h>
#include <gfx/upload.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
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
	uint32_t firstInstance = 0; // see mesh::Submesh::firstInstance
	uint32_t instanceCount = 1;
	uint32_t mirroredInstanceCount = 0;
	int32_t skin = -1; // see mesh::Submesh::skin
	uint32_t morphTargetCount = 0; // see mesh::Submesh::morphTargetCount
	uint32_t morphDeltaBase = 0;
	uint32_t morphFirstVertex = 0;
	uint32_t morphWeightBase = 0;
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
	float specular = 1.0F; // see mesh::Material::specular, and the fields below
	TextureRef specularTexture;
	std::array<float, 3> specularColor{1.0F, 1.0F, 1.0F};
	TextureRef specularColorTexture;
	float ior = 1.5F;
	bool specularGlossiness = false;
	float glossiness = 1.0F;
	// see mesh::Material::clearcoat and sheenColor
	float clearcoat = 0.0F;
	TextureRef clearcoatTexture;
	float clearcoatRoughness = 0.0F;
	TextureRef clearcoatRoughnessTexture;
	TextureRef clearcoatNormalTexture;
	float clearcoatNormalScale = 1.0F;
	std::array<float, 3> sheenColor{};
	TextureRef sheenColorTexture;
	float sheenRoughness = 0.0F;
	TextureRef sheenRoughnessTexture;
	// see mesh::Material::transmission, thickness and dispersion
	float transmission = 0.0F;
	TextureRef transmissionTexture;
	float thickness = 0.0F;
	TextureRef thicknessTexture;
	std::array<float, 3> attenuationColor{1.0F, 1.0F, 1.0F};
	float attenuationDistance = 0.0F;
	float dispersion = 0.0F;
	// see mesh::Material::anisotropy and iridescence
	float anisotropy = 0.0F;
	float anisotropyRotation = 0.0F;
	TextureRef anisotropyTexture;
	float iridescence = 0.0F;
	TextureRef iridescenceTexture;
	float iridescenceIor = 1.3F;
	float iridescenceThicknessMin = 100.0F;
	float iridescenceThicknessMax = 400.0F;
	TextureRef iridescenceThicknessTexture;
	// see mesh::Material::diffuseTransmission
	float diffuseTransmission = 0.0F;
	TextureRef diffuseTransmissionTexture;
	std::array<float, 3> diffuseTransmissionColor{1.0F, 1.0F, 1.0F};
	TextureRef diffuseTransmissionColorTexture;
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
	// the file's scenes and which one this is (see mesh::Mesh::scenes). none for a set of several files
	std::vector<std::string> scenes;
	uint32_t scene = 0;
	// the file's cameras (see mesh::Mesh::cameras). none for a set of several files (see Model::Load)
	std::vector<SceneCamera> cameras;
	// the file's lights (see mesh::Mesh::lights). none for a set of several files, which get the default light
	std::vector<SceneLight> lights;
	// what moves (see mesh::Mesh::animation). empty for a set of several files, which are drawn at rest
	SceneAnimationData animation;
	bool skinned = false; // whether it has skin vertices (SkinVertex, vertexCount of them, in their own buffer)
	uint32_t morphDeltaCount = 0; // MorphDelta in their own buffer (see mesh::Mesh::morphDeltas)
	std::vector<float> morphWeights; // the default morph weights (see mesh::Mesh::morphWeights)
};

// a model's gpu buffers: the instance and joint buffers are one per frame (host visible, see Model::Animate) if it moves,
// else one device local instance buffer and no joints
struct ModelBuffers
{
	Buffer index;
	Buffer vertex;
	Buffer skin; // SkinVertex per vertex, if ModelDesc::skinned
	std::vector<Buffer> instances; // ModelInstance per ModelDesc::instances
	std::vector<Buffer> joints; // ModelDesc::animation.jointCount joint matrices (at least one)
	Buffer morphDeltas; // MorphDelta, ModelDesc::morphDeltaCount of them, if any
	std::vector<Buffer> morphWeights; // per frame, host visible: ModelDesc::morphWeights' count of weights, if any
};

// a mesh on the gpu: its index, vertex and instance buffers, drawn a submesh (material, topology and instances) at a time
class Model final
{
public:
	Model(ModelDesc&& desc, ModelBuffers&& buffers, const Upload& upload) noexcept;
	Model(const Model&) = delete;
	Model(Model&&) noexcept = delete;
	~Model();

	Model& operator=(const Model&) = delete;
	Model& operator=(Model&&) noexcept = delete;

	[[nodiscard]] const ModelDesc& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] const Buffer& GetIndexBuffer() const noexcept { return myBuffers.index; }
	[[nodiscard]] const Buffer& GetVertexBuffer() const noexcept { return myBuffers.vertex; }
	// SkinVertex per vertex, or null if it has no skinned submeshes
	[[nodiscard]] const Buffer* GetSkinBuffer() const noexcept { return myBuffers.skin.IsValid() ? &myBuffers.skin : nullptr; }
	// ModelInstance (gfx/shaders/capi.h) per ModelDesc::instances, for a frame: the transform and its inverse transpose
	[[nodiscard]] const Buffer& GetInstanceBuffer(size_t frameIndex) const noexcept
	{
		return myBuffers.instances[myBuffers.instances.size() == 1 ? 0 : frameIndex];
	}
	// its morph deltas, and a frame's morph weights (see MorphDelta in gfx/shaders/capi.h), or null if it has none
	[[nodiscard]] const Buffer* GetMorphDeltaBuffer() const noexcept { return myBuffers.morphDeltas.IsValid() ? &myBuffers.morphDeltas : nullptr; }
	[[nodiscard]] const Buffer* GetMorphWeightBuffer(size_t frameIndex) const noexcept
	{
		return frameIndex < myBuffers.morphWeights.size() ? &myBuffers.morphWeights[frameIndex] : nullptr;
	}
	// the joint matrices for a frame, or null if it doesn't move
	[[nodiscard]] const Buffer* GetJointBuffer(size_t frameIndex) const noexcept
	{
		return frameIndex < myBuffers.joints.size() ? &myBuffers.joints[frameIndex] : nullptr;
	}
	// the buffers the upload filled, which the graphics queue acquires (see GetUpload)
	[[nodiscard]] std::vector<const Buffer*> GetUploadedBuffers() const;
	[[nodiscard]] bool Moves() const noexcept { return !myDesc.animation.Empty(); }

	// writes a frame's instance and joint buffers with a pose (see ScenePose: an animation at a time, looping, or the rest
	// pose), crossfaded in over from by weight (see EvaluateNodes; 1: pose alone). call on the draw thread, once the
	// frame's previous use of its buffers is done.
	void Animate(size_t frameIndex, const ScenePose& pose, const ScenePose& from = {}, float weight = 1.0F);
	// the upload of its buffers, which gpu work that uses them must wait for and acquire them from
	[[nodiscard]] const Upload& GetUpload() const noexcept { return myUpload; }

	// loads a model file (.obj, .gltf or .glb) through the asset cache (see core::file::LoadAsset and mesh::Import) and
	// uploads it on the
	// primary device's transfer queue. returns once the upload is submitted (see GetUpload), or null if the load was
	// cancelled because the application is exiting, or failed (the reason is printed to stderr).
	// scene: a gltf file's scene to load (see mesh::ImportOptions::scene), else its default one.
	[[nodiscard]] static std::shared_ptr<Model> Load(
		std::string_view filePath, std::atomic_uint8_t& progress, std::optional<size_t> scene = std::nullopt);
	// loads several model files (each through its own cache entry) as one, side by side in a grid in the xy plane, each
	// scaled to the same size and centered in its cell: for sets of variants, such as an archive's models, or the
	// encodings of a gltf model. files the importers don't support (see mesh::Unsupported) are skipped. null if any
	// load is cancelled or fails.
	[[nodiscard]] static std::shared_ptr<Model> Load(std::span<const std::string_view> filePaths, std::atomic_uint8_t& progress);

private:
	ModelDesc myDesc;
	ModelBuffers myBuffers;
	Upload myUpload;
	// the joint matrices Animate writes, kept between its calls
	std::vector<SceneMatrix> myJoints;
};

} // namespace gfx
