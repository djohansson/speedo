#include "../model.h"
#include "../shaders/capi.h"

#include <core/file.h>
#include <gfx/aabb.h>
#include <gfx/vertex.h>
#include <rhi/utils.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <zpp_bits.h>

auto serialize(const ModelCreateDesc<Vk>&) -> zpp::bits::members<4>;
auto serialize(const AABB<float>&) -> zpp::bits::members<2>;

namespace model
{

std::vector<VkVertexInputBindingDescription> CalculateInputBindingDescriptions(
	const std::vector<VertexInputAttributeDescription<Vk>>& attributes)
{
	using AttributeMap = std::map<uint32_t, std::tuple<VkFormat, uint32_t>>;

	AttributeMap attributeMap;

	for (const auto& attribute : attributes)
	{
		ASSERT(attribute.binding == 0); // todo: please implement me

		attributeMap[attribute.location] = std::make_tuple(attribute.format, attribute.offset);
	}

	//int32_t lastBinding = -1;
	int32_t lastLocation = -1;
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

		lastSize = getFormatSize(format);
		lastOffset = offset;

		stride = lastOffset + lastSize;
	}

	// ASSERT(VK_VERTEX_INPUT_RATE_VERTEX); // todo: please implement me

	return {VertexInputBindingDescription<Vk>{0U, stride, VK_VERTEX_INPUT_RATE_VERTEX}};
}

std::tuple<
	ModelCreateDesc<Vk>,
	BufferHandle<Vk>,
	AllocationHandle<Vk>,
	BufferHandle<Vk>,
	AllocationHandle<Vk>>
Load(
	const std::filesystem::path& modelFile,
	const std::shared_ptr<Device<Vk>>& device,
	std::atomic_uint8_t& progress)
{
	ZoneScopedN("model::load");

	std::tuple<
		ModelCreateDesc<Vk>,
		BufferHandle<Vk>,
		AllocationHandle<Vk>,
		BufferHandle<Vk>,
		AllocationHandle<Vk>> descAndInitialData;

	auto& [desc, ibHandle, ibMemHandle, vbHandle, vbMemHandle] = descAndInitialData;

	auto loadBin = [&descAndInitialData, &device, &progress](auto& in) -> std::error_code
	{
		ZoneScopedN("model::loadBin");

		progress = 32;

		auto& [desc, ibHandle, ibMemHandle, vbHandle, vbMemHandle] = descAndInitialData;
		
		if (auto result = in(desc); failure(result))
			return std::make_error_code(result);

		auto [locIbHandle, locIbMemHandle] = createBuffer(
			device->getAllocator(),
			desc.indexCount * sizeof(uint32_t),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			"todo_insert_proper_name");

		void* ibData;
		VK_CHECK(vmaMapMemory(device->getAllocator(), locIbMemHandle, &ibData));
		auto ibResult = in(std::span(static_cast<char*>(ibData), desc.indexCount * sizeof(uint32_t)));
		vmaUnmapMemory(device->getAllocator(), locIbMemHandle);
		if (failure(ibResult))
			return std::make_error_code(ibResult);

		ibHandle = locIbHandle;
		ibMemHandle = locIbMemHandle;

		progress = 128;

		auto [locVbHandle, locVbMemHandle] = createBuffer(
			device->getAllocator(),
			desc.vertexCount * sizeof(Vertex_P3f_N3f_T014f_C4f),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			"todo_insert_proper_name");

		void* vbData;
		VK_CHECK(vmaMapMemory(device->getAllocator(), locVbMemHandle, &vbData));
		auto vbResult = in(std::span(static_cast<char*>(vbData), desc.vertexCount * sizeof(Vertex_P3f_N3f_T014f_C4f)));
		vmaUnmapMemory(device->getAllocator(), locVbMemHandle);
		if (failure(vbResult))
			return std::make_error_code(vbResult);

		vbHandle = locVbHandle;
		vbMemHandle = locVbMemHandle;

		progress = 255;

		return {};
	};

	auto saveBin = [&descAndInitialData, &device, &progress](auto& out) -> std::error_code
	{
		ZoneScopedN("model::saveBin");

		auto& [desc, ibHandle, ibMemHandle, vbHandle, vbMemHandle] = descAndInitialData;
		
		if (auto result = out(desc); failure(result))
			return std::make_error_code(result);

		void* ibData;
		VK_CHECK(vmaMapMemory(device->getAllocator(), ibMemHandle, &ibData));
		auto ibResult = out(std::span(static_cast<const char*>(ibData), desc.indexCount * sizeof(uint32_t)));
		vmaUnmapMemory(device->getAllocator(), ibMemHandle);
		if (failure(ibResult))
			return std::make_error_code(ibResult);

		void* vbData;
		VK_CHECK(vmaMapMemory(device->getAllocator(), vbMemHandle, &vbData));
		auto vbResult = out(std::span(static_cast<const char*>(vbData), desc.vertexCount * sizeof(Vertex_P3f_N3f_T014f_C4f)));
		vmaUnmapMemory(device->getAllocator(), vbMemHandle);
		if (failure(vbResult))
			return std::make_error_code(vbResult);

		progress = 255;

		return {};
	};

	auto loadOBJ = [&descAndInitialData, &device, &modelFile, &progress](auto& /*todo: use me: in*/) -> std::error_code
	{
		ZoneScopedN("model::loadOBJ");

		progress = 32;

		auto& [desc, ibHandle, ibMemHandle, vbHandle, vbMemHandle] = descAndInitialData;

		using namespace tinyobj;
		attrib_t attrib;
		std::vector<shape_t> shapes;
		std::vector<material_t> materials;
		std::string warn;
		std::string err;
		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, modelFile.string().c_str()))
			throw std::runtime_error(err);

		progress = 64;

		uint32_t indexCount = 0;
		for (const auto& shape : shapes)
			indexCount += shape.mesh.indices.size();

		if (!attrib.vertices.empty())
		{
			desc.attributes.emplace_back(
				VertexInputAttributeDescription<Vk>{
					static_cast<uint32_t>(desc.attributes.size()),
					0,
					VK_FORMAT_R32G32B32_SFLOAT,
					offsetof(Vertex_P3f_N3f_T014f_C4f, position)});
		}

		if (!attrib.normals.empty())
		{
			desc.attributes.emplace_back(
				VertexInputAttributeDescription<Vk>{
					static_cast<uint32_t>(desc.attributes.size()),
					0, 
					VK_FORMAT_R32G32B32_SFLOAT,
					offsetof(Vertex_P3f_N3f_T014f_C4f, normal)});
		}

		if (!attrib.texcoords.empty())
		{
			desc.attributes.emplace_back(
				VertexInputAttributeDescription<Vk>{
					static_cast<uint32_t>(desc.attributes.size()),
					0,
					VK_FORMAT_R32G32B32A32_SFLOAT,
					offsetof(Vertex_P3f_N3f_T014f_C4f, texCoord01)});
		}

		if (!attrib.colors.empty())
		{
			desc.attributes.emplace_back(
				VertexInputAttributeDescription<Vk>{
					static_cast<uint32_t>(desc.attributes.size()),
					0,
					VK_FORMAT_R32G32B32A32_SFLOAT,
					offsetof(Vertex_P3f_N3f_T014f_C4f, color)});
		}

		UnorderedMap<uint64_t, uint32_t> uniqueVertices;

		VertexAllocator vertices;
		vertices.setStride(sizeof(Vertex_P3f_N3f_T014f_C4f));

		std::vector<uint32_t> indices;

		ScopedVertexAllocation vertexScope(vertices);
		vertices.reserve(indexCount / 3); // guesstimate
		indices.reserve(indexCount);

		for (const auto& shape : shapes)
		{
			for (const auto& index : shape.mesh.indices)
			{
				auto& vertex = *vertexScope.createVertices();
				
				if (!attrib.vertices.empty())
					std::copy_n(
						&attrib.vertices[3 * index.vertex_index],
						3,
						&vertex.dataAs<float>(offsetof(Vertex_P3f_N3f_T014f_C4f, position)));

				if (!attrib.normals.empty())
					std::copy_n(
						&attrib.normals[3 * index.normal_index],
						3,
						&vertex.dataAs<float>(offsetof(Vertex_P3f_N3f_T014f_C4f, normal)));

				if (!attrib.texcoords.empty())
				{
					float uvs[2] = {
						attrib.texcoords[2 * index.texcoord_index + 0],
						1.0F - attrib.texcoords[2 * index.texcoord_index + 1]};
					std::copy_n(
						uvs,
						2,
						&vertex.dataAs<float>(offsetof(Vertex_P3f_N3f_T014f_C4f, texCoord01)));
				}

				if (!attrib.colors.empty())
					std::copy_n(
						&attrib.colors[3 * index.vertex_index + 0],
						3,
						&vertex.dataAs<float>(offsetof(Vertex_P3f_N3f_T014f_C4f, color)));

				uint64_t vertexIndex = vertex.hash();
				if (uniqueVertices.count(vertexIndex) == 0)
				{
					uniqueVertices[vertexIndex] = static_cast<uint32_t>(vertices.size() - 1);

					if (!attrib.vertices.empty())
						desc.aabb.merge(
							vertex.dataAs<decltype(Vertex_P3f_N3f_T014f_C4f::position)>(offsetof(Vertex_P3f_N3f_T014f_C4f, position)));
				}
				else
				{
					vertexScope.freeVertices(&vertex);
				}
				indices.push_back(uniqueVertices[vertexIndex]);
			}
		}

		progress = 128;

		desc.indexCount = indices.size();
		desc.vertexCount = vertices.size();

		auto [locIbHandle, locIbMemHandle] = createBuffer(
			device->getAllocator(),
			desc.indexCount * sizeof(uint32_t),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			"todo_insert_proper_name");

		void* ibData;
		VK_CHECK(vmaMapMemory(device->getAllocator(), locIbMemHandle, &ibData));
		memcpy(ibData, indices.data(), desc.indexCount * sizeof(uint32_t));
		vmaUnmapMemory(device->getAllocator(), locIbMemHandle);

		ibHandle = locIbHandle;
		ibMemHandle = locIbMemHandle;

		progress = 192;

		auto [locVbHandle, locVbMemHandle] = createBuffer(
			device->getAllocator(),
			desc.vertexCount * sizeof(Vertex_P3f_N3f_T014f_C4f),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			"todo_insert_proper_name");

		void* vbData;
		VK_CHECK(vmaMapMemory(device->getAllocator(), locVbMemHandle, &vbData));
		memcpy(vbData, vertices.data(), desc.vertexCount * sizeof(Vertex_P3f_N3f_T014f_C4f));
		vmaUnmapMemory(device->getAllocator(), locVbMemHandle);

		vbHandle = locVbHandle;
		vbMemHandle = locVbMemHandle;

		progress = 224;

		return {};
	};

	static constexpr char loaderType[] = "tinyobjloader";
	static constexpr char loaderVersion[] = "2.0.15";
	file::LoadAsset<loaderType, loaderVersion>(modelFile, loadOBJ, loadBin, saveBin);

	if ((vbHandle == nullptr) || (ibHandle == nullptr))
		throw std::runtime_error("Failed to load model.");

	return descAndInitialData;
}

} // namespace model

template <>
Model<Vk>::Model(
	const std::shared_ptr<Device<Vk>>& device,
	Queue<Vk>& queue,
	uint64_t timelineValue,
	std::tuple<
		ModelCreateDesc<Vk>,
		BufferHandle<Vk>,
		AllocationHandle<Vk>,
		BufferHandle<Vk>,
		AllocationHandle<Vk>>&& descAndInitialData)
	: myDesc(std::forward<ModelCreateDesc<Vk>>(std::get<0>(descAndInitialData)))
	, myIndexBuffer(
		  device,
		  queue,
		  timelineValue,
		  std::make_tuple(
			  BufferCreateDesc<Vk>{
				  std::get<0>(descAndInitialData).indexCount * sizeof(uint32_t),
				  VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
			  std::get<1>(descAndInitialData),
			  std::get<2>(descAndInitialData)))
	, myVertexBuffer(
		  device,
		  queue,
		  timelineValue,
		  std::make_tuple(
			  BufferCreateDesc<Vk>{
				  std::get<0>(descAndInitialData).vertexCount * sizeof(Vertex_P3f_N3f_T014f_C4f),
				  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
			  std::get<3>(descAndInitialData),
			  std::get<4>(descAndInitialData)))
	, myBindings(model::CalculateInputBindingDescriptions(myDesc.attributes))
{}

template <>
Model<Vk>::Model(
	const std::shared_ptr<Device<Vk>>& device,
	Queue<Vk>& queue,
	uint64_t timelineValue,
	const std::filesystem::path& modelFile,
	std::atomic_uint8_t& progress)
	: Model(device, queue, timelineValue, model::Load(modelFile, device, progress))
{}

template <>
void Model<Vk>::Swap(Model& rhs) noexcept
{
	std::swap(myDesc, rhs.myDesc);
	std::swap(myIndexBuffer, rhs.myIndexBuffer);
	std::swap(myVertexBuffer, rhs.myVertexBuffer);
	std::swap(myBindings, rhs.myBindings);
}
