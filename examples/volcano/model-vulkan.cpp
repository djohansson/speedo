#include "model.h"
#include "aabb.h"
#include "file.h"
#include "vertex.h"
#include "vk-utils.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

//#define TINYOBJLOADER_USE_EXPERIMENTAL
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
#	define TINYOBJ_LOADER_OPT_IMPLEMENTATION
#	include <experimental/tinyobj_loader_opt.h>
#else
#	define TINYOBJLOADER_IMPLEMENTATION
#	include <tiny_obj_loader.h>
#endif

#include <cereal/archives/binary.hpp>

namespace model
{

std::vector<VkVertexInputBindingDescription> calculateInputBindingDescriptions(
	const std::vector<VertexInputAttributeDescription<Vk>>& attributes)
{
	using AttributeMap = MapType<uint32_t, std::pair<VkFormat, uint32_t>>;

	AttributeMap attributeMap;

	for (const auto& attribute : attributes)
	{
		assert(attribute.binding == 0); // todo: please implement me

		attributeMap[attribute.location] = std::make_pair(attribute.format, attribute.offset);
	}

	//int32_t lastBinding = -1;
	int32_t lastLocation = -1;
	uint32_t lastOffset = 0;
	uint32_t lastSize = 0;

	uint32_t stride = 0;

	for (const auto& [location, formatAndOffset] : attributeMap)
	{
		if (location != (lastLocation + 1))
			return {};

		lastLocation = location;

		if (formatAndOffset.second < (lastOffset + lastSize))
			return {};

		lastSize = getFormatSize(formatAndOffset.first);
		lastOffset = formatAndOffset.second;

		stride = lastOffset + lastSize;
	}

	// assert(VK_VERTEX_INPUT_RATE_VERTEX); // todo: please implement me

	return {VertexInputBindingDescription<Vk>{0u, stride, VK_VERTEX_INPUT_RATE_VERTEX}};
}

std::tuple<ModelCreateDesc<Vk>,	BufferHandle<Vk>, AllocationHandle<Vk>> load(
	const std::filesystem::path& modelFile,
	const std::shared_ptr<DeviceContext<Vk>>& deviceContext)
{
	ZoneScopedN("model::load");

	std::tuple<
		ModelCreateDesc<Vk>,
		BufferHandle<Vk>,
		AllocationHandle<Vk>> descAndInitialData = {};

	auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
    desc.name = modelFile.filename().generic_string();
	
	auto loadBin = [&descAndInitialData, &deviceContext](std::istream& stream)
	{
		ZoneScopedN("model::loadBin");

		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
		cereal::BinaryInputArchive bin(stream);
		bin(desc);

		std::string debugString;
		debugString.append(desc.name);
		debugString.append("_staging");
		
		auto [locBufferHandle, locMemoryHandle] = createBuffer(
			deviceContext->getAllocator(), desc.indexBufferSize + desc.vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			debugString.c_str());

		{
			ZoneScopedN("model::loadBin::buffers");

			std::byte* data;
			VK_CHECK(vmaMapMemory(deviceContext->getAllocator(), locMemoryHandle, (void**)&data));
			bin(cereal::binary_data(data, desc.indexBufferSize + desc.vertexBufferSize));
			vmaUnmapMemory(deviceContext->getAllocator(), locMemoryHandle);

			bufferHandle = locBufferHandle;
			memoryHandle = locMemoryHandle;
		}

		
		return true;
	};

	auto saveBin = [&descAndInitialData, &deviceContext](std::ostream& stream)
	{
		ZoneScopedN("model::saveBin");

		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
		cereal::BinaryOutputArchive bin(stream);
		bin(desc);

		{
			ZoneScopedN("model::saveBin::buffers");

			std::byte* data;
			VK_CHECK(vmaMapMemory(deviceContext->getAllocator(), memoryHandle, (void**)&data));
			bin(cereal::binary_data(data, desc.indexBufferSize + desc.vertexBufferSize));
			vmaUnmapMemory(deviceContext->getAllocator(), memoryHandle);
		}

		return true;
	};

	auto loadOBJ = [&descAndInitialData, &deviceContext](std::istream& stream)
	{
		ZoneScopedN("model::loadOBJ");

		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
		using namespace tinyobj_opt;
#else
		using namespace tinyobj;
#endif
		attrib_t attrib;
		std::vector<shape_t> shapes;
		std::vector<material_t> materials;
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
		std::streambuf* raw_buffer = stream.rdbuf();
		std::streamsize bufferSize = stream.gcount();
		std::unique_ptr<char[]> buffer = std::make_unique<char[]>(bufferSize);
		raw_buffer->sgetn(buffer.get(), bufferSize);
		LoadOption option;
		if (!parseObj(&attrib, &shapes, &materials, buffer.get(), bufferSize, option))
			throw std::runtime_error("Failed to load model.");
#else
		std::string warn, err;
		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, &stream))
			throw std::runtime_error(err);
#endif

		uint32_t indexCount = 0;
		for (const auto& shape : shapes)
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
			for (uint32_t faceOffset = shape.face_offset;
					faceOffset < (shape.face_offset + shape.length);
					faceOffset++)
				indexCount += attrib.face_num_verts[faceOffset];
#else
			indexCount += shape.mesh.indices.size();
#endif

		// todo: read this from file
		desc.attributes.emplace_back(
			VertexInputAttributeDescription<Vk>{0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 0u});
		desc.attributes.emplace_back(
			VertexInputAttributeDescription<Vk>{1u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 12u});
		desc.attributes.emplace_back(
			VertexInputAttributeDescription<Vk>{2u, 0u, VK_FORMAT_R32G32_SFLOAT, 24u});

		MapType<uint64_t, uint32_t> uniqueVertices;

		VertexAllocator vertices;
		vertices.setStride(32);

		std::vector<uint32_t> indices;

		ScopedVertexAllocation vertexScope(vertices);
		vertices.reserve(indexCount / 3); // guesstimate
		indices.reserve(indexCount);

		size_t posOffset = desc.attributes[0].offset;
		size_t colorOffset = desc.attributes[1].offset;
		size_t texCoordOffset = desc.attributes[2].offset;

		for (const auto& shape : shapes)
		{
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
			for (uint32_t faceOffset = shape.face_offset;
					faceOffset < (shape.face_offset + shape.length);
					faceOffset++)
			{
				const index_t& index = attrib.indices[faceOffset];
#else
			for (const auto& index : shape.mesh.indices)
			{
#endif
				auto& vertex = *vertexScope.createVertices();

				glm::vec3* pos = vertex.dataAs<glm::vec3>(posOffset);
				*pos = {attrib.vertices[3 * index.vertex_index + 0],
						attrib.vertices[3 * index.vertex_index + 1],
						attrib.vertices[3 * index.vertex_index + 2]};

				glm::vec3* color = vertex.dataAs<glm::vec3>(colorOffset);
				*color = {1.0f, 1.0f, 1.0f};

				glm::vec2* texCoord = vertex.dataAs<glm::vec2>(texCoordOffset);
				*texCoord = {attrib.texcoords[2 * index.texcoord_index + 0],
								1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};

				uint64_t vertexHash = vertex.hash();

				if (uniqueVertices.count(vertexHash) == 0)
				{
					uniqueVertices[vertexHash] = static_cast<uint32_t>(vertices.size() - 1);
					desc.aabb.merge(*pos);
				}
				else
				{
					vertexScope.freeVertices(&vertex);
				}

				indices.push_back(uniqueVertices[vertexHash]);
			}
		}

		desc.indexBufferSize = indices.size() * sizeof(uint32_t);
		desc.vertexBufferSize = vertices.sizeBytes();
		desc.indexCount = indices.size();

		std::string debugString;
		debugString.append(desc.name);
		debugString.append("_staging");
		
		auto [locBufferHandle, locMemoryHandle] = createBuffer(
			deviceContext->getAllocator(), desc.indexBufferSize + desc.vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			debugString.c_str());

		{
			ZoneScopedN("model::loadObj::buffers");

			std::byte* data;
			VK_CHECK(vmaMapMemory(deviceContext->getAllocator(), locMemoryHandle, (void**)&data));
			memcpy(data, indices.data(), desc.indexBufferSize);
			memcpy(data + desc.indexBufferSize, vertices.data(), desc.vertexBufferSize);
			vmaUnmapMemory(deviceContext->getAllocator(), locMemoryHandle);

			bufferHandle = locBufferHandle;
			memoryHandle = locMemoryHandle;
		}

		return true;
	};

	loadCachedSourceFile(
		modelFile, modelFile, "tinyobjloader", "1.4.0", loadOBJ, loadBin, saveBin);

	if (!bufferHandle)
		throw std::runtime_error("Failed to load model.");

	return descAndInitialData;
}

} // namespace model

template <>
Model<Vk>::Model(
	const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
	const std::shared_ptr<CommandContext<Vk>>& commandContext,
	std::tuple<ModelCreateDesc<Vk>, BufferHandle<Vk>, AllocationHandle<Vk>>&& descAndInitialData)
: myDesc(std::move(std::get<0>(descAndInitialData)))
, myBuffer(
	deviceContext,
	commandContext,
	std::make_tuple(
		BufferCreateDesc<Vk>{ { std::get<0>(descAndInitialData).name },
			std::get<0>(descAndInitialData).indexBufferSize + std::get<0>(descAndInitialData).vertexBufferSize,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT },
		std::get<1>(descAndInitialData),
		std::get<2>(descAndInitialData)))
, myBindings(model::calculateInputBindingDescriptions(myDesc.attributes))
{
}

template <>
Model<Vk>::Model(
	const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
	const std::shared_ptr<CommandContext<Vk>>& commandContext,
	const std::filesystem::path& modelFile)
	: Model(deviceContext, commandContext, model::load(modelFile, deviceContext))
{
}
