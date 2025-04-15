#include "../model.h"
#include "../rhiapplication.h"
#include "../shaders/capi.h"
#include "utils.h"

#include <core/file.h>
#include <core/std_extra.h>
#include <gfx/bounds.h>
#include <gfx/vertex.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <zpp_bits.h>

//NOLINTBEGIN(readability-identifier-naming)
[[nodiscard]] zpp::bits::members<std_extra::member_count<ModelCreateDesc<kVk>>()> serialize(const ModelCreateDesc<kVk>&);
[[nodiscard]] zpp::bits::members<std_extra::member_count<Bounds3f>()> serialize(const Bounds3f&);
//NOLINTEND(readability-identifier-naming)


namespace model::detail
{

std::vector<VkVertexInputBindingDescription> CalculateInputBindingDescriptions(
	const std::vector<VertexInputAttributeDescription<kVk>>& attributes)
{
	using AttributeMap = std::map<uint32_t, std::tuple<VkFormat, uint32_t>>;

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

	return {VertexInputBindingDescription<kVk>{0U, stride, VK_VERTEX_INPUT_RATE_VERTEX}};
}

//NOLINTBEGIN(readability-magic-numbers)
std::tuple<
	BufferHandle<kVk>,
	AllocationHandle<kVk>,
	BufferHandle<kVk>,
	AllocationHandle<kVk>,
	ModelCreateDesc<kVk>>
Load(
	const std::filesystem::path& modelFile,
	const std::shared_ptr<Device<kVk>>& device,
	std::atomic_uint8_t& progress)
{
	ZoneScopedN("model::load");

	std::tuple<
		BufferHandle<kVk>,
		AllocationHandle<kVk>,
		BufferHandle<kVk>,
		AllocationHandle<kVk>,
		ModelCreateDesc<kVk>> initialData;

	auto& [ibHandle, ibMemHandle, vbHandle, vbMemHandle, desc] = initialData;

	auto loadBin = [&modelFile, &initialData, &device, &progress](auto& inStream) -> std::error_code
	{
		ZoneScopedN("model::loadBin");

		progress = 32;

		auto& [ibHandle, ibMemHandle, vbHandle, vbMemHandle, desc] = initialData;
		
		if (auto result = inStream(desc); failure(result))
			return std::make_error_code(result);

		std::string ibName;
		std::string vbName;
		ibName = modelFile.filename().string().append("_staging_ib");
		vbName = modelFile.filename().string().append("_staging_vb");
		
		auto [locIbHandle, locIbMemHandle] = CreateBuffer(
			device->GetAllocator(),
			desc.indexCount * sizeof(uint32_t),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			ibName.data());

		void* ibData;
		VK_ENSURE(vmaMapMemory(device->GetAllocator(), locIbMemHandle, &ibData));
		auto ibResult = inStream(std::span(static_cast<char*>(ibData), desc.indexCount * sizeof(uint32_t)));
		vmaUnmapMemory(device->GetAllocator(), locIbMemHandle);
		if (failure(ibResult))
			return std::make_error_code(ibResult);

		ibHandle = locIbHandle;
		ibMemHandle = locIbMemHandle;

		progress = 128;

		auto [locVbHandle, locVbMemHandle] = CreateBuffer(
			device->GetAllocator(),
			desc.vertexCount * sizeof(VertexP3fN3fT014fC4f),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vbName.data());

		void* vbData;
		VK_ENSURE(vmaMapMemory(device->GetAllocator(), locVbMemHandle, &vbData));
		auto vbResult = inStream(
			std::span(static_cast<char*>(vbData), desc.vertexCount * sizeof(VertexP3fN3fT014fC4f)));
		vmaUnmapMemory(device->GetAllocator(), locVbMemHandle);
		if (failure(vbResult))
			return std::make_error_code(vbResult);

		vbHandle = locVbHandle;
		vbMemHandle = locVbMemHandle;

		progress = 255;

		return {};
	};

	auto saveBin = [&initialData, &device, &progress](auto& out) -> std::error_code
	{
		ZoneScopedN("model::saveBin");

		auto& [ibHandle, ibMemHandle, vbHandle, vbMemHandle, desc] = initialData;
		
		if (auto result = out(desc); failure(result))
			return std::make_error_code(result);

		void* ibData;
		VK_ENSURE(vmaMapMemory(device->GetAllocator(), ibMemHandle, &ibData));
		auto ibResult = out(std::span(static_cast<const char*>(ibData), desc.indexCount * sizeof(uint32_t)));
		vmaUnmapMemory(device->GetAllocator(), ibMemHandle);
		if (failure(ibResult))
			return std::make_error_code(ibResult);

		void* vbData;
		VK_ENSURE(vmaMapMemory(device->GetAllocator(), vbMemHandle, &vbData));
		auto vbResult = out(std::span(
			static_cast<const char*>(vbData), desc.vertexCount * sizeof(VertexP3fN3fT014fC4f)));
		vmaUnmapMemory(device->GetAllocator(), vbMemHandle);
		if (failure(vbResult))
			return std::make_error_code(vbResult);

		progress = 255;

		return {};
	};

	auto loadOBJ = [&modelFile, &initialData, &device, &progress](auto& /*todo: use me: in*/) -> std::error_code
	{
		ZoneScopedN("model::loadOBJ");

		progress = 32;

		auto& [ibHandle, ibMemHandle, vbHandle, vbMemHandle, desc] = initialData;

		using namespace tinyobj;
		attrib_t attrib;
		std::vector<shape_t> shapes;
		std::vector<material_t> materials;
		std::string warn;
		std::string err;
		ENSUREF(tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, modelFile.string().c_str()), "%s", err)

		progress = 64;

		uint32_t indexCount = 0;
		for (const auto& shape : shapes)
			indexCount += shape.mesh.indices.size();

		if (!attrib.vertices.empty())
		{
			desc.attributes.emplace_back(VertexInputAttributeDescription<kVk>{
				static_cast<uint32_t>(desc.attributes.size()),
				0,
				VK_FORMAT_R32G32B32_SFLOAT,
				static_cast<uint32_t>(offsetof(VertexP3fN3fT014fC4f, position))});
		}

		if (!attrib.normals.empty())
		{
			desc.attributes.emplace_back(VertexInputAttributeDescription<kVk>{
				static_cast<uint32_t>(desc.attributes.size()),
				0,
				VK_FORMAT_R32G32B32_SFLOAT,
				static_cast<uint32_t>(offsetof(VertexP3fN3fT014fC4f, normal))});
		}

		if (!attrib.texcoords.empty())
		{
			desc.attributes.emplace_back(VertexInputAttributeDescription<kVk>{
				static_cast<uint32_t>(desc.attributes.size()),
				0,
				VK_FORMAT_R32G32B32A32_SFLOAT,
				static_cast<uint32_t>(offsetof(VertexP3fN3fT014fC4f, texCoord01))});
		}

		if (!attrib.colors.empty())
		{
			desc.attributes.emplace_back(VertexInputAttributeDescription<kVk>{
				static_cast<uint32_t>(desc.attributes.size()),
				0,
				VK_FORMAT_R32G32B32A32_SFLOAT,
				static_cast<uint32_t>(offsetof(VertexP3fN3fT014fC4f, color))});
		}

		UnorderedMap<uint64_t, uint32_t> uniqueVertices;

		VertexAllocator vertices;
		vertices.SetStride(sizeof(VertexP3fN3fT014fC4f));

		std::vector<uint32_t> indices;

		ScopedVertexAllocation vertexScope(vertices);
		vertices.Reserve(indexCount / 3); // guesstimate
		indices.reserve(indexCount);

		for (const auto& shape : shapes)
		{
			for (const auto& index : shape.mesh.indices)
			{
				auto& vertex = *vertexScope.CreateVertices();
				
				if (!attrib.vertices.empty())
					std::copy_n(
						&attrib.vertices[3UL * index.vertex_index],
						3,
						&vertex.DataAs<float>(offsetof(VertexP3fN3fT014fC4f, position)));

				if (!attrib.normals.empty())
					std::copy_n(
						&attrib.normals[3UL * index.normal_index],
						3,
						&vertex.DataAs<float>(offsetof(VertexP3fN3fT014fC4f, normal)));

				if (!attrib.texcoords.empty())
				{
					std::array<float, 2> uvs = {
						attrib.texcoords[2UL * index.texcoord_index],
						1.0F - attrib.texcoords[(2UL * index.texcoord_index) + 1]};
					std::copy_n(
						uvs.data(), uvs.size(), &vertex.DataAs<float>(offsetof(VertexP3fN3fT014fC4f, texCoord01)));
				}

				if (!attrib.colors.empty())
					std::copy_n(
						&attrib.colors[3UL * index.vertex_index],
						3,
						&vertex.DataAs<float>(offsetof(VertexP3fN3fT014fC4f, color)));

				uint64_t vertexIndex = vertex.Hash();
				if (!uniqueVertices.contains(vertexIndex))
				{
					uniqueVertices[vertexIndex] = static_cast<uint32_t>(vertices.Size() - 1);

					if (!attrib.vertices.empty())
						desc.bounds.Merge(
							std::to_array(vertex.DataAs<decltype(VertexP3fN3fT014fC4f::position)>(offsetof(VertexP3fN3fT014fC4f, position))));
				}
				else
				{
					vertexScope.FreeVertices(&vertex);
				}
				indices.push_back(uniqueVertices[vertexIndex]);
			}
		}

		progress = 128;

		std::string ibName;
		std::string vbName;
		ibName = modelFile.filename().string().append("_staging_ib");
		vbName = modelFile.filename().string().append("_staging_vb");

		desc.indexCount = indices.size();
		desc.vertexCount = vertices.Size();

		auto [locIbHandle, locIbMemHandle] = CreateBuffer(
			device->GetAllocator(),
			desc.indexCount * sizeof(uint32_t),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			ibName.data());

		void* ibData;
		VK_ENSURE(vmaMapMemory(device->GetAllocator(), locIbMemHandle, &ibData));
		memcpy(ibData, indices.data(), desc.indexCount * sizeof(uint32_t));
		vmaUnmapMemory(device->GetAllocator(), locIbMemHandle);

		ibHandle = locIbHandle;
		ibMemHandle = locIbMemHandle;

		progress = 192;

		auto [locVbHandle, locVbMemHandle] = CreateBuffer(
			device->GetAllocator(),
			desc.vertexCount * sizeof(VertexP3fN3fT014fC4f),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vbName.data());

		void* vbData;
		VK_ENSURE(vmaMapMemory(device->GetAllocator(), locVbMemHandle, &vbData));
		memcpy(vbData, vertices.Data(), desc.vertexCount * sizeof(VertexP3fN3fT014fC4f));
		vmaUnmapMemory(device->GetAllocator(), locVbMemHandle);

		vbHandle = locVbHandle;
		vbMemHandle = locVbMemHandle;

		progress = 224;

		return {};
	};

	std::string params;
	std::string paramsHash;
	params.append("tinyobjloader-2.0.15"); // todo: read version from tinyobjloader.h
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	auto loadResult = file::LoadAsset(modelFile, loadOBJ, loadBin, saveBin, paramsHash);

	ENSUREF(loadResult && vbHandle != nullptr && ibHandle != nullptr, "Failed to load model.");

	return initialData;
}
//NOLINTEND(readability-magic-numbers)

} // namespace model::detail

template <>
Model<kVk>::Model(
	const std::shared_ptr<Device<kVk>>& device,
	std::array<TaskCreateInfo<void>, 2>& timelineCallbacksOut,
	CommandBufferHandle<kVk> cmd,
	std::tuple<
		BufferHandle<kVk>,
		AllocationHandle<kVk>,
		BufferHandle<kVk>,
		AllocationHandle<kVk>,
		ModelCreateDesc<kVk>>&& initialData)
	: myIndexBuffer(
		  device,
		  timelineCallbacksOut[0],
		  cmd,
		  std::make_tuple(
			  std::get<0>(initialData),
			  std::get<1>(initialData),
			  BufferCreateDesc<kVk>{
				  .size = std::get<4>(initialData).indexCount * sizeof(uint32_t),
				  .usageFlags = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				  .memoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				  .name = "IndexBuffer"}))
	, myVertexBuffer(
		  device,
		  timelineCallbacksOut[1],
		  cmd,
		  std::make_tuple(
			  std::get<2>(initialData),
			  std::get<3>(initialData),
			  BufferCreateDesc<kVk>{
				  .size = std::get<4>(initialData).vertexCount * sizeof(VertexP3fN3fT014fC4f),
				  .usageFlags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				  .memoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				  .name = "VertexBuffer"}))
	, myBindings(model::detail::CalculateInputBindingDescriptions(std::get<4>(initialData).attributes))
	, myDesc(std::forward<ModelCreateDesc<kVk>>(std::get<4>(initialData)))
{}

template <>
Model<kVk>::Model(
	const std::shared_ptr<Device<kVk>>& device,
	std::array<TaskCreateInfo<void>, 2>& timelineCallbacksOut,
	CommandBufferHandle<kVk> cmd,
	const std::filesystem::path& modelFile,
	std::atomic_uint8_t& progress)
	: Model(device, timelineCallbacksOut, cmd, model::detail::Load(modelFile, device, progress))
{}

template <>
void Model<kVk>::Swap(Model& rhs) noexcept
{
	std::swap(myIndexBuffer, rhs.myIndexBuffer);
	std::swap(myVertexBuffer, rhs.myVertexBuffer);
	std::swap(myBindings, rhs.myBindings);
	std::swap(myDesc, rhs.myDesc);
}

namespace model
{

template <>
Model<kVk> LoadModel(std::string_view filePath, std::atomic_uint8_t& progress, std::shared_ptr<Model<kVk>> oldModel)
{
	ZoneScopedN("model::LoadModel");

	auto app = std::static_pointer_cast<RHIApplication>(Application::Get().lock());
	ENSURE(app);
	auto& rhi = app->GetRHI<kVk>();
	
	auto transfer = ConcurrentWriteScope(rhi.queues[kQueueTypeTransfer]);
	auto& [transferQueue, transferSubmit] = transfer->queues.Get();

	std::array<TaskCreateInfo<void>, 2> transfersDone;
	auto model = Model<kVk>(
		rhi.device,
		transfersDone,
		transferQueue.GetPool().Commands(),
		filePath,
		progress);

 	// a bit cryptic, but it's just a task that holds on to the old model in its capture group until task is destroyed
	auto [oldModelDestroyTask, oldModelDestroyFuture] = CreateTask([model = std::move(oldModel)] {});

	std::vector<TaskHandle> timelineCallbacks;
	timelineCallbacks.emplace_back(transfersDone[0].handle);
	timelineCallbacks.emplace_back(transfersDone[1].handle);
	timelineCallbacks.emplace_back(oldModelDestroyTask);

	transferQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
		.waitSemaphores = {transfer->semaphore},
		.waitDstStageMasks = {VK_PIPELINE_STAGE_TRANSFER_BIT},
		.waitSemaphoreValues = {transferSubmit.maxTimelineValue},
		.signalSemaphores = {transfer->semaphore},
		.signalSemaphoreValues = {++transfer->timeline},
		.callbacks = std::move(timelineCallbacks)});

	transferSubmit = transferQueue.Submit();

	return model;
}

} // namespace model
