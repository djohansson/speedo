#include <gfx/model.h>
#include <gfx/objimport.h>

#include <gfx/shaders/capi.h>

#include <core/application.h>
#include <core/file.h>
#include <core/profiling.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace gfx
{

Model::Model(ModelDesc&& desc, Buffer&& indexBuffer, Buffer&& vertexBuffer) noexcept
	: myDesc(std::move(desc))
	, myIndexBuffer(std::move(indexBuffer))
	, myVertexBuffer(std::move(vertexBuffer))
{}

Model::~Model() = default;

//NOLINTBEGIN(readability-magic-numbers)
std::shared_ptr<Model> Model::Load(std::string_view filePath, std::atomic_uint8_t& progress)
{
	using namespace rhi;

	ZoneScopedN("gfx::Model::Load");

	auto* rhi = GetRHI<kGraphicsApi>();
	ENSURE(rhi);
	auto& device = rhi->GetPrimaryDevice();

	// loading is given up when the application exits (only where it takes long; parsing itself can't be interrupted)
	auto app = core::Application::Get();
	ENSURE(app);
	auto cancelled = [&app] { return app->IsExitRequested(); };

	// the vertex buffer is read by the shaders as a storage buffer, which can't be larger than this
	auto fitsDevice = [&device, filePath](const ModelDesc& desc)
	{
		auto limits = device.GetLimits();
		auto vertexBufferSize = static_cast<uint64_t>(desc.vertexCount) * sizeof(VertexP3fN3fT014fC4f);
		if (vertexBufferSize <= limits.maxStorageBufferRange)
			return true;

		std::println(
			stderr, "{}: the vertex buffer ({} bytes) is larger than the device's maxStorageBufferRange ({} bytes)",
			filePath, vertexBufferSize, limits.maxStorageBufferRange);
		return false;
	};

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
			desc.vertexCount * sizeof(VertexP3fN3fT014fC4f));
	};
	auto releaseStaging = [&]
	{
		indexStaging = {};
		vertexStaging = {};
	};

	auto loadBin = [&](auto& inStream) -> std::error_code
	{
		ZoneScopedN("gfx::Model::loadBin");

		progress = 32;

		if (auto result = inStream(desc); failure(result))
			return std::make_error_code(result);

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

	auto loadOBJ = [&](auto& /*todo: use me: in*/) -> std::error_code
	{
		ZoneScopedN("gfx::Model::loadOBJ");

		progress = 32;

		auto mesh = obj::Import(std::filesystem::path(filePath), cancelled);
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
		for (const auto& submesh : mesh->submeshes)
			desc.submeshes.push_back({.firstIndex = submesh.firstIndex, .indexCount = submesh.indexCount, .material = submesh.material});
		for (const auto& material : mesh->materials)
			desc.materials.push_back({
				.name = material.name,
				.diffuseTexture = material.diffuseTexture.string(),
				.alphaTexture = material.alphaTexture.string(),
				.normalTexture = material.normalTexture.string(),
				.bumpTexture = material.bumpTexture.string(),
				.bumpScale = material.bumpScale});

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
	params.append("tinyobjloader-2.0.0"); // todo: read version from tinyobjloader.h
	params.append("|objimport-v1"); // bump when obj::Import changes what it produces
	params.append("|cache-v6"); // bump when the serialized layout (ModelDesc) changes, to invalidate stale caches
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	// the materials are baked into the vertex colors, so a change to them must reimport the model
	auto materialFiles = [filePath] { return obj::MaterialFiles(std::filesystem::path(filePath)); };
	auto loadResult = core::file::LoadAsset(filePath, loadOBJ, loadBin, saveBin, paramsHash, materialFiles, &progress, cancelled);

	if (!loadResult || !indexStaging.IsValid() || !vertexStaging.IsValid())
	{
		// cancelled or failed (the staging buffers are released with it)
		if (!loadResult && loadResult.error() != std::errc::operation_canceled)
			std::println(stderr, "Failed to load model {}: {}", filePath, loadResult.error().message());

		return {};
	}

	desc.name = std::string(filePath);

	std::shared_ptr<Model> model;
	const Semaphore* transferSemaphore = nullptr;
	uint64_t transferTimelineValue = 0;
	{
		auto transfer = device.GetQueue(kQueueTypeTransfer).Write();
		auto& [transferQueue, transferSubmits] = transfer->queues.Get();

		auto cmd = transferQueue.GetPool().Commands();

		std::array<core::TaskCreateInfo<void>, 2> transfersDone;
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
				desc.vertexCount * sizeof(VertexP3fN3fT014fC4f),
				BufferUsage::kVertex | BufferUsage::kStorage | BufferUsage::kTransferDestination,
				MemoryProperty::kDeviceLocal},
			std::move(vertexStaging),
			cmd,
			transfersDone[1]);
		model = std::make_shared<Model>(std::move(desc), std::move(indexBuffer), std::move(vertexBuffer));
		cmd.End();

		std::vector<core::TaskHandle> timelineCallbacks;
		timelineCallbacks.emplace_back(transfersDone[0].handle);
		timelineCallbacks.emplace_back(transfersDone[1].handle);

		transferTimelineValue = ++transfer->timeline;
		transferQueue.EnqueueSubmit(QueueDeviceSyncInfo{
			.waitSemaphores = {transfer->semaphore},
			.waitDstStageMasks = {PipelineStage::kTransfer},
			.waitSemaphoreValues = {transferSubmits.maxTimelineValue},
			.signalSemaphores = {transfer->semaphore},
			.signalSemaphoreValues = {transferTimelineValue},
			.callbacks = std::move(timelineCallbacks)});

		transferSubmits |= transferQueue.Submit();

		transferSemaphore = &transfer->semaphore;
	}

	// wait for the upload outside the queue lock, so the model is ready for use by the caller
	transferSemaphore->Wait(transferTimelineValue);

	return model;
}
//NOLINTEND(readability-magic-numbers)

} // namespace gfx
