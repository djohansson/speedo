#include <gfx/model.h>
#include <gfx/importversions.h>
#include <gfx/meshimport.h>

#include <gfx/shaders/capi.h>

#include <core/application.h>
#include <core/file.h>
#include <core/profiling.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace gfx
{

Model::Model(ModelDesc&& desc, Buffer&& indexBuffer, Buffer&& vertexBuffer, Buffer&& instanceBuffer, const Upload& upload) noexcept
	: myDesc(std::move(desc))
	, myIndexBuffer(std::move(indexBuffer))
	, myVertexBuffer(std::move(vertexBuffer))
	, myInstanceBuffer(std::move(instanceBuffer))
	, myUpload(upload)
{}

Model::~Model() = default;

//NOLINTBEGIN(readability-magic-numbers)
namespace model
{

// a model's desc, and its indices and vertices in staging buffers, before the upload
struct Staged
{
	ModelDesc desc;
	Buffer indexStaging;
	Buffer vertexStaging;
};

// the vertex and instance buffers are read by the shaders as storage buffers, which can't be larger than this
[[nodiscard]] static bool FitsDevice(const Device& device, const ModelDesc& desc, std::string_view name)
{
	auto limits = device.GetLimits();
	for (auto [buffer, size] : {
			 std::pair{"vertex", static_cast<uint64_t>(desc.vertexCount) * sizeof(VertexP3fN3fTa4fT014fC4f)},
			 std::pair{"instance", static_cast<uint64_t>(desc.instances.size()) * sizeof(ModelInstance)}})
	{
		if (size > limits.maxStorageBufferRange)
		{
			std::println(
				stderr, "{}: the {} buffer ({} bytes) is larger than the device's maxStorageBufferRange ({} bytes)", name,
				buffer, size, limits.maxStorageBufferRange);
			return false;
		}
	}
	return true;
}

// loads a model file through the asset cache into staging buffers, filled before the upload takes the transfer
// queue's lock. nothing if cancelled or failed (the reason is printed to stderr).
[[nodiscard]] static std::optional<Staged> LoadStaged(Device& device, std::string_view filePath, std::atomic_uint8_t& progress)
{
	using namespace rhi;

	ZoneScopedN("gfx::Model::LoadStaged");

	// loading is given up when the application exits (only where it takes long; parsing itself can't be interrupted)
	auto app = core::Application::Get();
	ENSURE(app);
	auto cancelled = [&app] { return app->IsExitRequested(); };

	auto fitsDevice = [&device, filePath](const ModelDesc& desc) { return FitsDevice(device, desc, filePath); };

	// the indices and vertices, in staging buffers: filled here, before the upload takes the transfer queue's lock
	ModelDesc desc;
	Buffer indexStaging;
	Buffer vertexStaging;
	auto createStaging = [&]
	{
		indexStaging = Buffer::CreateStaging(
			device.CreateDeviceObjectCreateDesc(std::format("{} (index staging)", filePath)), desc.indexCount * sizeof(uint32_t));
		vertexStaging = Buffer::CreateStaging(
			device.CreateDeviceObjectCreateDesc(std::format("{} (vertex staging)", filePath)),
			desc.vertexCount * sizeof(VertexP3fN3fTa4fT014fC4f));
	};
	auto releaseStaging = [&]
	{
		indexStaging = {};
		vertexStaging = {};
	};

	// embedded images of gltf files are extracted to files, which the textures are loaded from
	auto userProfilePath = std::get<std::filesystem::path>(app->GetEnv().variables["UserProfilePath"]);
	auto absolutePath = std::filesystem::absolute(std::filesystem::path(filePath));
	mesh::ImportOptions importOptions{
		.embeddedImageDirectory = userProfilePath / "embedded" /
								  std::format("{}-{:016x}", absolutePath.stem().string(), std::hash<std::string>{}(absolutePath.string()))};

	// the embedded images a cached model names, which only an import writes: missing ones (e.g. a cleared user profile)
	// make the cache unusable, so that LoadAsset imports the model again, which extracts them again
	auto missingEmbeddedImage = [&importOptions](const ModelDesc& modelDesc) -> std::optional<std::string>
	{
		auto directory = importOptions.embeddedImageDirectory.generic_string() + "/";
		for (const auto& material : modelDesc.materials)
			for (const auto* texture : {&material.diffuseTexture, &material.alphaTexture, &material.normalTexture,
										&material.bumpTexture, &material.emissiveTexture, &material.occlusionTexture})
				if (std::error_code error; std::filesystem::path(texture->path).generic_string().starts_with(directory) &&
										   !std::filesystem::is_regular_file(texture->path, error))
					return texture->path;
		return std::nullopt;
	};

	auto loadBin = [&](auto& inStream) -> std::error_code
	{
		ZoneScopedN("gfx::Model::loadBin");

		progress = 32;

		if (auto result = inStream(desc); failure(result))
			return std::make_error_code(result);

		if (auto missing = missingEmbeddedImage(desc))
		{
			std::println(stderr, "{}: embedded image {} is missing, importing again", filePath, *missing);
			desc = {};
			return std::make_error_code(std::errc::no_such_file_or_directory);
		}

		if (!fitsDevice(desc))
			return std::make_error_code(std::errc::file_too_large);

		createStaging();

		for (auto* staging : {&indexStaging, &vertexStaging})
		{
			auto memory = staging->Map();
			auto result = inStream(std::span(reinterpret_cast<char*>(memory.data()), memory.size()));
			staging->Unmap();
			if (failure(result))
			{
				releaseStaging();
				return std::make_error_code(result);
			}

			progress = staging == &indexStaging ? 128 : 255;
		}

		return {};
	};

	auto saveBin = [&](auto& outStream) -> std::error_code
	{
		ZoneScopedN("gfx::Model::saveBin");

		if (auto result = outStream(desc); failure(result))
			return std::make_error_code(result);

		for (auto* staging : {&indexStaging, &vertexStaging})
		{
			auto memory = staging->Map();
			auto result = outStream(std::span(reinterpret_cast<const char*>(memory.data()), memory.size()));
			staging->Unmap();
			if (failure(result))
				return std::make_error_code(result);
		}

		// LoadAsset reports the rest, while hashing the saved cache
		return {};
	};

	auto loadModel = [&](auto& /*todo: use me: in*/) -> std::error_code
	{
		ZoneScopedN("gfx::Model::loadModel");

		progress = 32;

		auto mesh = mesh::Import(std::filesystem::path(filePath), importOptions, cancelled);
		if (!mesh)
		{
			if (cancelled())
				return std::make_error_code(std::errc::operation_canceled);

			std::println(stderr, "{}", mesh.error());
			return std::make_error_code(std::errc::invalid_argument);
		}

		for (const auto& warning : mesh->stats.warnings)
			std::println(stderr, "{}: {}", filePath, warning);

		if (mesh->indices.empty())
		{
			std::println(stderr, "{}: no triangles", filePath);
			return std::make_error_code(std::errc::invalid_argument);
		}

		progress = 128;

		desc.bounds = mesh->bounds;
		desc.indexCount = static_cast<uint32_t>(mesh->indices.size());
		desc.vertexCount = static_cast<uint32_t>(mesh->vertices.size());
		desc.instances = mesh->instances;
		for (const auto& submesh : mesh->submeshes)
		{
			auto& modelSubmesh = desc.submeshes.emplace_back(ModelSubmesh{
				.firstIndex = submesh.firstIndex,
				.indexCount = submesh.indexCount,
				.material = submesh.material,
				.topology = submesh.topology,
				.firstInstance = submesh.firstInstance,
				.instanceCount = submesh.instanceCount,
				.mirroredInstanceCount = submesh.mirroredInstanceCount});
			if (submesh.indexCount == 0)
				continue;
			// in world space: the center of its bounds' corners at each of its instances
			glm::vec3 min(std::numeric_limits<float>::max());
			glm::vec3 max(std::numeric_limits<float>::lowest());
			for (auto index : std::span(mesh->indices).subspan(submesh.firstIndex, submesh.indexCount))
			{
				auto position = glm::make_vec3(mesh->vertices[index].position);
				min = glm::min(min, position);
				max = glm::max(max, position);
			}
			glm::vec3 worldMin(std::numeric_limits<float>::max());
			glm::vec3 worldMax(std::numeric_limits<float>::lowest());
			for (uint32_t instanceIt = submesh.firstInstance; instanceIt < submesh.firstInstance + submesh.instanceCount; instanceIt++)
				for (uint32_t corner = 0; corner < 8; corner++)
				{
					glm::vec4 local((corner & 1U) != 0 ? max.x : min.x, (corner & 2U) != 0 ? max.y : min.y, (corner & 4U) != 0 ? max.z : min.z, 1.0F);
					auto world = glm::vec3(glm::make_mat4(mesh->instances[instanceIt].data()) * local);
					worldMin = glm::min(worldMin, world);
					worldMax = glm::max(worldMax, world);
				}
			auto center = 0.5F * (worldMin + worldMax);
			modelSubmesh.center = {center.x, center.y, center.z};
		}
		for (const auto& material : mesh->materials)
			desc.materials.push_back({
				.name = material.name,
				.diffuseTexture = material.diffuseTexture,
				.alphaTexture = material.alphaTexture,
				.normalTexture = material.normalTexture,
				.normalScale = material.normalScale,
				.emissive = material.emissive,
				.emissiveTexture = material.emissiveTexture,
				.occlusionTexture = material.occlusionTexture,
				.occlusionStrength = material.occlusionStrength,
				.bumpTexture = material.bumpTexture,
				.bumpScale = material.bumpScale,
				.alphaCutoff = material.alphaCutoff,
				.doubleSided = material.doubleSided,
				.blend = material.blend});

		if (!fitsDevice(desc))
			return std::make_error_code(std::errc::file_too_large);

		createStaging();

		auto indices = indexStaging.Map();
		std::memcpy(indices.data(), mesh->indices.data(), indices.size());
		indexStaging.Unmap();

		progress = 192;

		auto vertices = vertexStaging.Map();
		std::memcpy(vertices.data(), mesh->vertices.data(), vertices.size());
		vertexStaging.Unmap();

		progress = 224;

		return {};
	};

	std::string params;
	std::string paramsHash;
	// bump an importer's tag when it changes what it produces
	if (auto extension = std::filesystem::path(filePath).extension().string(); extension == ".obj" || extension == ".OBJ")
		params.append(std::format("tinyobjloader-{}|objimport-v2", kTinyObjLoaderVersion));
	else
		params.append(std::format("cgltf-{}|gltfimport-v11", kCgltfVersion));
	params.append("|cache-v16"); // bump when the serialized layout (ModelDesc) changes, to invalidate stale caches
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	// the materials are baked into the vertex colors, so a change to them (or a gltf file's buffers) must reimport it
	auto dependencies = [filePath] { return mesh::Dependencies(std::filesystem::path(filePath)); };
	auto loadResult = core::file::LoadAsset(filePath, loadModel, loadBin, saveBin, paramsHash, dependencies, &progress, cancelled);

	if (!loadResult || !indexStaging.IsValid() || !vertexStaging.IsValid())
	{
		// cancelled or failed (the staging buffers are released with it)
		if (!loadResult && loadResult.error() != std::errc::operation_canceled)
			std::println(stderr, "Failed to load model {}: {}", filePath, loadResult.error().message());

		return std::nullopt;
	}

	desc.name = std::string(filePath);

	return Staged{.desc = std::move(desc), .indexStaging = std::move(indexStaging), .vertexStaging = std::move(vertexStaging)};
}

// the models side by side, in a grid in the xy plane (facing +z, the cameras' default view), as one: their indices and
// vertices in new staging buffers, and their materials and instances. each is scaled (uniformly, so its normals stay) to the same size
// and centered in its cell: the files of a set needn't share a scale (sphere.zip's spheres have radius 1 and 115).
[[nodiscard]] static std::optional<Staged> Merge(Device& device, std::vector<Staged>&& models, std::string name)
{
	ZoneScopedN("gfx::Model::Merge");

	if (models.size() == 1)
		return std::move(models.front());

	constexpr float kCell = 1.25F; // for models of size 1, with some space between them
	Staged merged;
	merged.desc.name = std::move(name);

	// placed by their instance transforms, which the vertices are drawn with: each model's become the placement times
	// its own, and its submeshes' instances move along
	auto columns = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(models.size()))));
	for (size_t modelIt = 0; modelIt < models.size(); modelIt++)
	{
		const auto& desc = models[modelIt].desc;

		// left to right, top to bottom
		auto column = static_cast<float>(modelIt % columns);
		auto row = static_cast<float>(modelIt / columns);
		auto center = desc.bounds.Center();
		auto size = desc.bounds.Size();
		auto extent = std::max({size.x, size.y, size.z});
		auto scale = extent > 0.0F ? 1.0F / extent : 1.0F;
		std::array<float, 3> offset{
			(column * kCell) - (center.x * scale), (-row * kCell) - (center.y * scale), -center.z * scale};
		auto place = [&](float value, size_t axis) { return (value * scale) + offset[axis]; };

		auto materialBase = static_cast<int32_t>(merged.desc.materials.size());
		auto instanceBase = static_cast<uint32_t>(merged.desc.instances.size());
		merged.desc.materials.insert(merged.desc.materials.end(), desc.materials.begin(), desc.materials.end());
		for (auto submesh : desc.submeshes)
		{
			submesh.firstIndex += merged.desc.indexCount;
			if (submesh.material >= 0)
				submesh.material += materialBase;
			submesh.firstInstance += instanceBase;
			for (size_t axis = 0; axis < 3; axis++)
				submesh.center[axis] = place(submesh.center[axis], axis);
			merged.desc.submeshes.push_back(submesh);
		}
		for (auto transform : desc.instances)
		{
			// the placement (a uniform scale and an offset) times the transform, column major
			for (size_t col = 0; col < 4; col++)
				for (size_t row = 0; row < 3; row++)
					transform[(col * 4) + row] = (scale * transform[(col * 4) + row]) + (offset[row] * transform[(col * 4) + 3]);
			merged.desc.instances.push_back(transform);
		}

		auto min = desc.bounds.GetMin();
		auto max = desc.bounds.GetMax();
		auto placedMin = Bounds3f::VectorType(place(min.x, 0), place(min.y, 1), place(min.z, 2));
		auto placedMax = Bounds3f::VectorType(place(max.x, 0), place(max.y, 1), place(max.z, 2));
		if (modelIt == 0)
		{
			merged.desc.bounds.SetMin(placedMin);
			merged.desc.bounds.SetMax(placedMax);
		}
		else
		{
			merged.desc.bounds.Merge(std::array{placedMin.x, placedMin.y, placedMin.z});
			merged.desc.bounds.Merge(std::array{placedMax.x, placedMax.y, placedMax.z});
		}

		merged.desc.indexCount += desc.indexCount;
		merged.desc.vertexCount += desc.vertexCount;
	}

	if (!FitsDevice(device, merged.desc, merged.desc.name))
		return std::nullopt;

	merged.indexStaging = Buffer::CreateStaging(
		device.CreateDeviceObjectCreateDesc(std::format("{} (index staging)", merged.desc.name)),
		merged.desc.indexCount * sizeof(uint32_t));
	merged.vertexStaging = Buffer::CreateStaging(
		device.CreateDeviceObjectCreateDesc(std::format("{} (vertex staging)", merged.desc.name)),
		merged.desc.vertexCount * sizeof(VertexP3fN3fTa4fT014fC4f));

	auto indices = merged.indexStaging.Map();
	auto vertices = merged.vertexStaging.Map();
	auto* indexOut = reinterpret_cast<uint32_t*>(indices.data());
	auto* vertexOut = reinterpret_cast<VertexP3fN3fTa4fT014fC4f*>(vertices.data());

	uint32_t vertexBase = 0;
	uint32_t indexBase = 0;
	for (auto& model : models)
	{
		const auto& desc = model.desc;

		auto sourceIndices = model.indexStaging.Map();
		const auto* indexIn = reinterpret_cast<const uint32_t*>(sourceIndices.data());
		for (uint32_t i = 0; i < desc.indexCount; i++)
			indexOut[indexBase + i] = vertexBase + indexIn[i];
		model.indexStaging.Unmap();

		auto sourceVertices = model.vertexStaging.Map();
		std::memcpy(&vertexOut[vertexBase], sourceVertices.data(), desc.vertexCount * sizeof(VertexP3fN3fTa4fT014fC4f));
		model.vertexStaging.Unmap();

		vertexBase += desc.vertexCount;
		indexBase += desc.indexCount;

		// the sources' staging memory goes as soon as it is copied
		model = {};
	}

	merged.indexStaging.Unmap();
	merged.vertexStaging.Unmap();

	return merged;
}

// uploads a model's staged indices and vertices on the transfer queue (see Model::Load)
[[nodiscard]] static std::shared_ptr<Model> UploadStaged(Device& device, Staged&& staged)
{
	using namespace rhi;

	ZoneScopedN("gfx::Model::Upload");

	auto& [desc, indexStaging, vertexStaging] = staged;
	std::string filePath = desc.name; // for the buffers' names: desc is moved into the model

	// each instance's transform and its inverse transpose (for the normals), filled before taking the queue's lock
	auto instanceStaging = Buffer::CreateStaging(
		device.CreateDeviceObjectCreateDesc(std::format("{} (instance staging)", filePath)),
		desc.instances.size() * sizeof(ModelInstance));
	{
		auto memory = instanceStaging.Map();
		auto* instances = reinterpret_cast<ModelInstance*>(memory.data());
		for (size_t instanceIt = 0; instanceIt < desc.instances.size(); instanceIt++)
		{
			auto transform = glm::make_mat4(desc.instances[instanceIt].data());
			auto inverseTranspose = glm::transpose(glm::inverse(transform));
			std::memcpy(&instances[instanceIt].modelTransform[0][0], glm::value_ptr(transform), sizeof(float) * 16);
			std::memcpy(&instances[instanceIt].inverseTransposeModelTransform[0][0], glm::value_ptr(inverseTranspose), sizeof(float) * 16);
		}
		instanceStaging.Unmap();
	}

	// one queue lock at a time: the queue types may alias the same context (see Device::GetQueue)
	auto graphicsQueueFamilyIndex = device.GetQueue(kQueueTypeGraphics).Read()->queueFamilyIndex;

	std::shared_ptr<Model> model;
	{
		auto transfer = device.GetQueue(kQueueTypeTransfer).Write();
		auto& [transferQueue, transferSubmits] = transfer->queues.Get();

		auto cmd = transferQueue.GetPool().Commands();

		std::array<core::TaskCreateInfo<void>, 3> transfersDone;
		auto indexBuffer = Buffer(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("{} (indices)", filePath)),
				desc.indexCount * sizeof(uint32_t),
				BufferUsage::kIndex | BufferUsage::kTransferDestination,
				MemoryProperty::kDeviceLocal},
			std::move(indexStaging),
			cmd,
			transfersDone[0]);
		auto vertexBuffer = Buffer(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("{} (vertices)", filePath)),
				desc.vertexCount * sizeof(VertexP3fN3fTa4fT014fC4f),
				BufferUsage::kVertex | BufferUsage::kStorage | BufferUsage::kTransferDestination,
				MemoryProperty::kDeviceLocal},
			std::move(vertexStaging),
			cmd,
			transfersDone[1]);
		auto instanceBuffer = Buffer(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("{} (instances)", filePath)),
				desc.instances.size() * sizeof(ModelInstance),
				BufferUsage::kStorage | BufferUsage::kTransferDestination,
				MemoryProperty::kDeviceLocal},
			std::move(instanceStaging),
			cmd,
			transfersDone[2]);
		auto upload = Upload{
			.semaphore = &transfer->semaphore, .value = ++transfer->timeline, .queueFamilyIndex = transfer->queueFamilyIndex};
		model = std::make_shared<Model>(
			std::move(desc), std::move(indexBuffer), std::move(vertexBuffer), std::move(instanceBuffer), upload);

		CommandEncoder encoder(cmd);
		for (const auto* buffer : {&model->GetIndexBuffer(), &model->GetVertexBuffer(), &model->GetInstanceBuffer()})
			encoder.ReleaseOwnership(
				*buffer, upload.queueFamilyIndex, graphicsQueueFamilyIndex, PipelineStage::kTransfer, Access::kTransferWrite);
		cmd.End();

		std::vector<core::TaskHandle> timelineCallbacks;
		timelineCallbacks.emplace_back(transfersDone[0].handle);
		timelineCallbacks.emplace_back(transfersDone[1].handle);
		timelineCallbacks.emplace_back(transfersDone[2].handle);
		// the caller may drop the model before the upload has completed, e.g. if the load it is part of is cancelled
		timelineCallbacks.emplace_back(core::CreateTask([model] {}).handle);

		transferQueue.EnqueueSubmit(QueueDeviceSyncInfo{
			.waitSemaphores = {transfer->semaphore},
			.waitDstStageMasks = {PipelineStage::kTransfer},
			.waitSemaphoreValues = {transferSubmits.maxTimelineValue},
			.signalSemaphores = {transfer->semaphore},
			.signalSemaphoreValues = {upload.value},
			.callbacks = std::move(timelineCallbacks)});

		transferSubmits |= transferQueue.Submit();
	}

	return model;
}

} // namespace model

std::shared_ptr<Model> Model::Load(std::string_view filePath, std::atomic_uint8_t& progress)
{
	return Load(std::span(&filePath, 1), progress);
}

std::shared_ptr<Model> Model::Load(std::span<const std::string_view> filePaths, std::atomic_uint8_t& progress)
{
	ZoneScopedN("gfx::Model::Load");

	ENSURE(!filePaths.empty());

	auto* rhi = rhi::GetRHI<rhi::kGraphicsApi>();
	ENSURE(rhi);
	auto& device = rhi->GetPrimaryDevice();

	// each file through its own cache entry: the progress of each is a share of the whole. in a set, files the
	// importers don't support (e.g. other encodings of a gltf model, see mesh::Unsupported) are left out.
	std::vector<model::Staged> models;
	models.reserve(filePaths.size());
	for (size_t fileIt = 0; fileIt < filePaths.size(); fileIt++)
	{
		if (filePaths.size() > 1)
		{
			if (auto reason = mesh::Unsupported(std::filesystem::path(filePaths[fileIt])))
			{
				std::println(stderr, "Skipped model {}: {}", filePaths[fileIt], *reason);
				continue;
			}
		}

		std::atomic_uint8_t fileProgress = 0;
		auto staged = model::LoadStaged(device, filePaths[fileIt], filePaths.size() == 1 ? progress : fileProgress);
		if (!staged) // cancelled or failed
			return {};
		models.push_back(std::move(*staged));
		progress = static_cast<uint8_t>(255 * (fileIt + 1) / filePaths.size());
	}
	if (models.empty())
	{
		std::println(stderr, "Failed to load model {}: none of its files are supported", filePaths.front());
		return {};
	}

	auto name = filePaths.size() == 1
					? std::string(filePaths.front())
					: std::format("{} ({} models)", std::filesystem::path(filePaths.front()).parent_path().string(), filePaths.size());
	auto merged = model::Merge(device, std::move(models), std::move(name));
	if (!merged)
		return {};

	return model::UploadStaged(device, std::move(*merged));
}
//NOLINTEND(readability-magic-numbers)

} // namespace gfx
