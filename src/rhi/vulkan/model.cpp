#include <rhi/model.h>
#include <rhi/rhi.h>
#include <rhi/rhiapplication.h>
#include <rhi/shaders/capi.h>
#include <rhi/vulkan/utils.h>

#include <core/file.h>
#include <gfx/objimport.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <print>
#include <tuple>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Model<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Model<kVk>);

namespace model
{

std::vector<VkVertexInputBindingDescription> CalculateInputBindingDescriptions(
	const std::vector<VertexInputAttributeDescription<kVk>>& attributes)
{
	using AttributeMap = core::UnorderedMap<uint32_t, std::tuple<Format<kVk>, uint32_t>>;

	AttributeMap attributeMap;

	for (const auto& attribute : attributes)
	{
		ASSERT(attribute.binding == 0); // todo: please implement me

		attributeMap[attribute.location] = std::make_tuple(attribute.format, attribute.offset);
	}

	//int32_t lastBinding = -1;
	int64_t lastLocation = -1;
	uint32_t lastOffset = 0;
	uint32_t lastSize = 0;

	uint32_t stride = 0;

	for (const auto& [location, formatAndOffset] : attributeMap)
	{
		const auto& [format, offset] = formatAndOffset;

		if (location != (lastLocation + 1))
			return {};

		lastLocation = location;

		if (offset < (lastOffset + lastSize))
			return {};

		lastSize = GetFormatSize(format);
		lastOffset = offset;

		stride = lastOffset + lastSize;
	}

	// ASSERT(VK_VERTEX_INPUT_RATE_VERTEX); // todo: please implement me

	return {VertexInputBindingDescription<kVk>{.binding = 0U, .stride = stride, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
}

//NOLINTBEGIN(readability-magic-numbers)
std::tuple<
	BufferHandle<kVk>,
	AllocationHandle<kVk>,
	BufferHandle<kVk>,
	AllocationHandle<kVk>,
	ModelCreateDesc<kVk>>
Load(
	ModelCreateDesc<kVk>&& desc,
	std::string_view modelFile,
	std::atomic_uint8_t& progressOut)
{
	ZoneScopedN("model::load");

	auto app = std::static_pointer_cast<RHIApplication>(core::Application::Get());
	ENSURE(app);
	auto& rhi = app->GetRHI<kVk>();
	auto& device = rhi.GetDevice(desc.device);

	// loading is given up when the application exits (only where it takes long; parsing itself can't be interrupted)
	auto cancelled = [&app] { return app->IsExitRequested(); };

	// the vertex buffer is read by the shaders as a storage buffer, which can't be larger than this
	auto fitsDevice = [&device, &modelFile](const ModelCreateDesc<kVk>& modelDesc)
	{
		const auto& limits = device.GetInstance().GetPhysicalDeviceInfo(device.GetPhysicalDevice()).deviceProperties.properties.limits;
		auto vertexBufferSize = static_cast<uint64_t>(modelDesc.vertexCount) * sizeof(VertexP3fN3fT014fC4f);
		if (vertexBufferSize <= limits.maxStorageBufferRange)
			return true;

		std::println(
			stderr, "{}: the vertex buffer ({} bytes) is larger than the device's maxStorageBufferRange ({} bytes)",
			modelFile, vertexBufferSize, limits.maxStorageBufferRange);
		return false;
	};

	auto initialData = std::tuple<
		BufferHandle<kVk>,
		AllocationHandle<kVk>,
		BufferHandle<kVk>,
		AllocationHandle<kVk>,
		ModelCreateDesc<kVk>>{{}, {}, {}, {}, std::forward<ModelCreateDesc<kVk>>(desc)};

	auto& [ibHandle, ibMemHandle, vbHandle, vbMemHandle, modelDesc] = initialData;

	auto loadBin = [&modelFile, &initialData, &device, &progressOut, &fitsDevice](auto& inStream) -> std::error_code
	{
		ZoneScopedN("model::loadBin");

		progressOut = 32;

		auto& [ibHandle, ibMemHandle, vbHandle, vbMemHandle, modelDesc] = initialData;
		
		if (auto result = inStream(modelDesc); failure(result))
			return std::make_error_code(result);

		// the cached desc holds the uuid of the model it was saved from (see ObjectCreateDesc)
		modelDesc.uuid = uuids::NewUuid();

		if (!fitsDevice(modelDesc))
			return std::make_error_code(std::errc::file_too_large);

		std::string ibName;
		std::string vbName;
		ibName = std::string(modelFile).append("_staging_ib");
		vbName = std::string(modelFile).append("_staging_vb");
		
		auto [locIbHandle, locIbMemHandle] = CreateBuffer(
			device.GetAllocator(),
			modelDesc.indexCount * sizeof(uint32_t),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			ibName.data());

		void* ibData;
		VK_CHECK(vmaMapMemory(device.GetAllocator(), locIbMemHandle, &ibData));
		auto ibResult = inStream(std::span(static_cast<char*>(ibData), modelDesc.indexCount * sizeof(uint32_t)));
		vmaUnmapMemory(device.GetAllocator(), locIbMemHandle);
		if (failure(ibResult))
		{
			DestroyBuffer(device.GetAllocator(), locIbHandle, locIbMemHandle);
			return std::make_error_code(ibResult);
		}

		progressOut = 128;

		auto [locVbHandle, locVbMemHandle] = CreateBuffer(
			device.GetAllocator(),
			modelDesc.vertexCount * sizeof(VertexP3fN3fT014fC4f),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vbName.data());

		void* vbData;
		VK_CHECK(vmaMapMemory(device.GetAllocator(), locVbMemHandle, &vbData));
		auto vbResult = inStream(
			std::span(static_cast<char*>(vbData), modelDesc.vertexCount * sizeof(VertexP3fN3fT014fC4f)));
		vmaUnmapMemory(device.GetAllocator(), locVbMemHandle);
		if (failure(vbResult))
		{
			DestroyBuffer(device.GetAllocator(), locVbHandle, locVbMemHandle);
			DestroyBuffer(device.GetAllocator(), locIbHandle, locIbMemHandle);
			return std::make_error_code(vbResult);
		}

		ibHandle = locIbHandle;
		ibMemHandle = locIbMemHandle;
		vbHandle = locVbHandle;
		vbMemHandle = locVbMemHandle;

		progressOut = 255;

		return {};
	};

	auto saveBin = [&initialData, &device](auto& out) -> std::error_code
	{
		ZoneScopedN("model::saveBin");

		auto& [ibHandle, ibMemHandle, vbHandle, vbMemHandle, modelDesc] = initialData;
		
		if (auto result = out(modelDesc); failure(result))
			return std::make_error_code(result);

		void* ibData;
		VK_CHECK(vmaMapMemory(device.GetAllocator(), ibMemHandle, &ibData));
		auto ibResult = out(std::span(static_cast<const char*>(ibData), modelDesc.indexCount * sizeof(uint32_t)));
		vmaUnmapMemory(device.GetAllocator(), ibMemHandle);
		if (failure(ibResult))
			return std::make_error_code(ibResult);

		void* vbData;
		VK_CHECK(vmaMapMemory(device.GetAllocator(), vbMemHandle, &vbData));
		auto vbResult = out(std::span(
			static_cast<const char*>(vbData), modelDesc.vertexCount * sizeof(VertexP3fN3fT014fC4f)));
		vmaUnmapMemory(device.GetAllocator(), vbMemHandle);
		if (failure(vbResult))
			return std::make_error_code(vbResult);

		// LoadAsset reports the rest, while hashing the saved cache
		return {};
	};

	auto loadOBJ = [&modelFile, &initialData, &device, &progressOut, &cancelled, &fitsDevice](auto& /*todo: use me: in*/) -> std::error_code
	{
		ZoneScopedN("model::loadOBJ");

		progressOut = 32;

		auto& [ibHandle, ibMemHandle, vbHandle, vbMemHandle, desc] = initialData;

		auto mesh = gfx::obj::Import(std::filesystem::path(modelFile), cancelled);
		if (!mesh)
		{
			if (cancelled())
				return std::make_error_code(std::errc::operation_canceled);

			std::println(stderr, "{}", mesh.error());
			return std::make_error_code(std::errc::invalid_argument);
		}

		for (const auto& warning : mesh->stats.warnings)
			std::println(stderr, "{}: {}", modelFile, warning);

		if (mesh->indices.empty())
		{
			std::println(stderr, "{}: no triangles", modelFile);
			return std::make_error_code(std::errc::invalid_argument);
		}

		progressOut = 128;

		// every vertex has all of them: the importer generates missing normals, and texcoords default to zero
		for (auto [format, offset] : {
				 std::pair{VK_FORMAT_R32G32B32_SFLOAT, offsetof(VertexP3fN3fT014fC4f, position)},
				 std::pair{VK_FORMAT_R32G32B32_SFLOAT, offsetof(VertexP3fN3fT014fC4f, normal)},
				 std::pair{VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VertexP3fN3fT014fC4f, texCoord01)},
				 std::pair{VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VertexP3fN3fT014fC4f, color)}})
		{
			desc.attributes.emplace_back(VertexInputAttributeDescription<kVk>{
				.location = static_cast<uint32_t>(desc.attributes.size()),
				.binding = 0,
				.format = format,
				.offset = static_cast<uint32_t>(offset)});
		}

		desc.bounds = mesh->bounds;
		for (const auto& submesh : mesh->submeshes)
			desc.submeshes.push_back({.firstIndex = submesh.firstIndex, .indexCount = submesh.indexCount, .material = submesh.material});
		for (const auto& material : mesh->materials)
			desc.materials.push_back({
				.name = material.name,
				.diffuseTexture = material.diffuseTexture.string(),
				.alphaTexture = material.alphaTexture.string(),
				.bumpTexture = material.bumpTexture.string(),
				.bumpScale = material.bumpScale});
		desc.indexCount = static_cast<uint32_t>(mesh->indices.size());
		desc.vertexCount = static_cast<uint32_t>(mesh->vertices.size());

		if (!fitsDevice(desc))
			return std::make_error_code(std::errc::file_too_large);

		std::string ibName;
		std::string vbName;
		ibName = std::string(modelFile).append("_staging_ib");
		vbName = std::string(modelFile).append("_staging_vb");

		auto [locIbHandle, locIbMemHandle] = CreateBuffer(
			device.GetAllocator(),
			desc.indexCount * sizeof(uint32_t),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			ibName.data());

		void* ibData;
		VK_CHECK(vmaMapMemory(device.GetAllocator(), locIbMemHandle, &ibData));
		memcpy(ibData, mesh->indices.data(), desc.indexCount * sizeof(uint32_t));
		vmaUnmapMemory(device.GetAllocator(), locIbMemHandle);

		ibHandle = locIbHandle;
		ibMemHandle = locIbMemHandle;

		progressOut = 192;

		auto [locVbHandle, locVbMemHandle] = CreateBuffer(
			device.GetAllocator(),
			desc.vertexCount * sizeof(VertexP3fN3fT014fC4f),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vbName.data());

		void* vbData;
		VK_CHECK(vmaMapMemory(device.GetAllocator(), locVbMemHandle, &vbData));
		memcpy(vbData, mesh->vertices.data(), desc.vertexCount * sizeof(VertexP3fN3fT014fC4f));
		vmaUnmapMemory(device.GetAllocator(), locVbMemHandle);

		vbHandle = locVbHandle;
		vbMemHandle = locVbMemHandle;

		progressOut = 224;

		return {};
	};

	std::string params;
	std::string paramsHash;
	params.append("tinyobjloader-2.0.0"); // todo: read version from tinyobjloader.h
	params.append("|objimport-v1"); // bump when gfx::obj::Import changes what it produces
	params.append("|cache-v4"); // bump when the serialized ModelCreateDesc layout changes, to invalidate stale caches
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	// the materials are baked into the vertex colors, so a change to them must reimport the model
	auto materialFiles = [&modelFile] { return gfx::obj::MaterialFiles(std::filesystem::path(modelFile)); };
	auto loadResult = core::file::LoadAsset(modelFile, loadOBJ, loadBin, saveBin, paramsHash, materialFiles, &progressOut, cancelled);

	if (!loadResult || vbHandle == nullptr || ibHandle == nullptr)
	{
		// cancelled or failed, possibly after creating staging buffers (e.g. while hashing the cache)
		if (!loadResult && loadResult.error() != std::errc::operation_canceled)
			std::println(stderr, "Failed to load model {}: {}", modelFile, loadResult.error().message());

		if (ibHandle != nullptr)
			DestroyBuffer(device.GetAllocator(), ibHandle, ibMemHandle);
		if (vbHandle != nullptr)
			DestroyBuffer(device.GetAllocator(), vbHandle, vbMemHandle);
		ibHandle = nullptr;
		vbHandle = nullptr;
	}

	return initialData;
}
//NOLINTEND(readability-magic-numbers)

} // namespace model

template <>
void Model<kVk>::Swap(Model& rhs) noexcept
{
	DeviceObject<Model<kVk>>::Swap(rhs);
	std::swap(myIndexBuffer, rhs.myIndexBuffer);
	std::swap(myVertexBuffer, rhs.myVertexBuffer);
	std::swap(myBindings, rhs.myBindings);
}

template <>
Model<kVk>::Model(
	std::tuple<
		BufferHandle<kVk>,
		AllocationHandle<kVk>,
		BufferHandle<kVk>,
		AllocationHandle<kVk>,
		CreateDescType>&& initialDataAndDesc,
	CommandBufferHandle<kVk> cmd,
	std::array<core::TaskCreateInfo<void>, 2>& timelineCallbacksOut)
	: DeviceObject<Model<kVk>>(std::forward<CreateDescType>(std::get<4>(initialDataAndDesc)))
	, myIndexBuffer(
		BufferCreateDesc<kVk>{
			SuperType::CreateDeviceObjectCreateDesc("IndexBuffer"),
			GetDesc().indexCount * sizeof(uint32_t),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
		std::make_tuple(
			std::get<0>(initialDataAndDesc),
			std::get<1>(initialDataAndDesc)),
		cmd,
		timelineCallbacksOut[0])
	, myVertexBuffer(
		BufferCreateDesc<kVk>{
			SuperType::CreateDeviceObjectCreateDesc("VertexBuffer"),
			GetDesc().vertexCount * sizeof(VertexP3fN3fT014fC4f),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
		std::make_tuple(
			std::get<2>(initialDataAndDesc),
			std::get<3>(initialDataAndDesc)),
		cmd,
		timelineCallbacksOut[1])
	, myBindings(model::CalculateInputBindingDescriptions(GetDesc().attributes))
{}

template <>
Model<kVk>::Model(
	CreateDescType&& desc,
	std::string_view modelFile,
	CommandBufferHandle<kVk> cmd,
	std::array<core::TaskCreateInfo<void>, 2>& timelineCallbacksOut,
	std::atomic_uint8_t& progressOut)
	: Model(
		model::Load(std::forward<CreateDescType>(desc), modelFile, progressOut),
		cmd,
		timelineCallbacksOut)
{}

template <>
std::shared_ptr<Model<kVk>> Model<kVk>::LoadModel(std::string_view filePath, std::atomic_uint8_t& progressOut)
{
	ZoneScopedN("Model::LoadModel");

	auto app = std::static_pointer_cast<RHIApplication>(core::Application::Get());
	ENSURE(app);
	auto& rhi = app->GetRHI<kVk>();
	auto& device = rhi.GetPrimaryDevice();

	// parse into staging buffers before taking the queue lock: on devices without a dedicated transfer queue it is the
	// graphics queue's lock, which Draw() takes every frame (see Device::GetQueue)
	auto initialDataAndDesc = model::Load(ModelCreateDesc<kVk>{device.CreateDeviceObjectCreateDesc(filePath)}, filePath, progressOut);
	if (std::get<0>(initialDataAndDesc) == nullptr) // cancelled or failed
		return {};

	std::shared_ptr<Model<kVk>> model;
	const Semaphore<kVk>* transferSemaphore = nullptr;
	uint64_t transferTimelineValue = 0;
	{
		auto transfer = device.GetQueue(kQueueTypeTransfer).Write();
		auto& [transferQueue, transferSubmits] = transfer->queues.Get();

		auto cmd = transferQueue.GetPool().Commands();

		std::array<core::TaskCreateInfo<void>, 2> transfersDone;
		// not make_shared: the constructor taking staging buffers is private
		model = std::shared_ptr<Model<kVk>>(new Model<kVk>(std::move(initialDataAndDesc), cmd, transfersDone));
		cmd.End();

		std::vector<core::TaskHandle> timelineCallbacks;
		timelineCallbacks.emplace_back(transfersDone[0].handle);
		timelineCallbacks.emplace_back(transfersDone[1].handle);

		transferTimelineValue = ++transfer->timeline;
		transferQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {transfer->semaphore},
			.waitDstStageMasks = {VK_PIPELINE_STAGE_TRANSFER_BIT},
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

} // namespace rhi
