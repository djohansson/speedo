#include "model.h"

#include "aabb.h"
#include "file.h"
#include "vertex.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
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
#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

namespace model
{

std::vector<VkVertexInputBindingDescription>
calculateInputBindingDescriptions(
	const std::vector<SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>>& attributes)
{
	using AttributeMap = std::map<uint32_t, std::pair<VkFormat, uint32_t>>;

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

	return {VertexInputBindingDescription<GraphicsBackend::Vulkan>{0u, stride,
																   VK_VERTEX_INPUT_RATE_VERTEX}};
}

ModelDesc<GraphicsBackend::Vulkan> load(
	const std::filesystem::path& modelFile,
	const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext)
{
	ModelDesc<GraphicsBackend::Vulkan> desc = {};
	desc.deviceContext = deviceContext;
	desc.debugName = modelFile.u8string();
	
	auto loadPBin = [&desc](std::istream& stream) {
		cereal::PortableBinaryInputArchive pbin(stream);
		pbin(desc.aabb, desc.attributes, desc.vertexBufferSize, desc.indexBufferSize, desc.indexCount);

		std::string vertexDebugString;
		vertexDebugString.append(desc.debugName);
		vertexDebugString.append("_vertex_staging");
		
		std::tie(desc.initialVertices, desc.initialVerticesMemory) = createBuffer(
			desc.deviceContext->getAllocator(), desc.vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vertexDebugString.c_str());

		std::byte* vertexData;
		CHECK_VK(vmaMapMemory(desc.deviceContext->getAllocator(), desc.initialVerticesMemory, (void**)&vertexData));
		pbin(cereal::binary_data(vertexData, desc.vertexBufferSize));
		vmaUnmapMemory(desc.deviceContext->getAllocator(), desc.initialVerticesMemory);
		
		std::string indexDebugString;
		indexDebugString.append(desc.debugName);
		indexDebugString.append("_index_staging");
		
		std::tie(desc.initialIndices, desc.initialIndicesMemory) = createBuffer(
			desc.deviceContext->getAllocator(), desc.indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			indexDebugString.c_str());

		std::byte* indexData;
		CHECK_VK(vmaMapMemory(desc.deviceContext->getAllocator(), desc.initialIndicesMemory, (void**)&indexData));
		pbin(cereal::binary_data(indexData, desc.indexBufferSize));
		vmaUnmapMemory(desc.deviceContext->getAllocator(), desc.initialIndicesMemory);
	};

	auto savePBin = [&desc](std::ostream& stream) {
		cereal::PortableBinaryOutputArchive pbin(stream);
		pbin(desc.aabb, desc.attributes, desc.vertexBufferSize, desc.indexBufferSize, desc.indexCount);

		std::byte* vertexData;
		CHECK_VK(vmaMapMemory(desc.deviceContext->getAllocator(), desc.initialVerticesMemory, (void**)&vertexData));
		pbin(cereal::binary_data(vertexData, desc.vertexBufferSize));
		vmaUnmapMemory(desc.deviceContext->getAllocator(), desc.initialVerticesMemory);

		std::byte* indexData;
		CHECK_VK(vmaMapMemory(desc.deviceContext->getAllocator(), desc.initialIndicesMemory, (void**)&indexData));
		pbin(cereal::binary_data(indexData, desc.indexBufferSize));
		vmaUnmapMemory(desc.deviceContext->getAllocator(), desc.initialIndicesMemory);
	};

	auto loadOBJ = [&desc](std::istream& stream) {
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
			SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>{{0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 0u}});
		desc.attributes.emplace_back(
			SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>{{1u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 12u}});
		desc.attributes.emplace_back(
			SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>{{2u, 0u, VK_FORMAT_R32G32_SFLOAT, 24u}});

		std::unordered_map<uint64_t, uint32_t> uniqueVertices;

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

		desc.vertexBufferSize = vertices.sizeBytes();
		desc.indexBufferSize = indices.size() * sizeof(uint32_t);
		desc.indexCount = indices.size();

		std::string vertexDebugString;
		vertexDebugString.append(desc.debugName);
		vertexDebugString.append("_vertex_staging");
		
		std::tie(desc.initialVertices, desc.initialVerticesMemory) = createBuffer(
			desc.deviceContext->getAllocator(), desc.vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vertexDebugString.c_str());

		std::byte* vertexData;
		CHECK_VK(vmaMapMemory(desc.deviceContext->getAllocator(), desc.initialVerticesMemory, (void**)&vertexData));
		memcpy(vertexData, vertices.data(), desc.vertexBufferSize);
		vmaUnmapMemory(desc.deviceContext->getAllocator(), desc.initialVerticesMemory);
		
		std::string indexDebugString;
		indexDebugString.append(desc.debugName);
		indexDebugString.append("_index_staging");
		
		std::tie(desc.initialIndices, desc.initialIndicesMemory) = createBuffer(
			desc.deviceContext->getAllocator(), desc.indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			indexDebugString.c_str());

		std::byte* indexData;
		CHECK_VK(vmaMapMemory(desc.deviceContext->getAllocator(), desc.initialIndicesMemory, (void**)&indexData));
		memcpy(indexData, indices.data(), desc.indexBufferSize);
		vmaUnmapMemory(desc.deviceContext->getAllocator(), desc.initialIndicesMemory);
	};

	loadCachedSourceFile(
		modelFile, modelFile, "tinyobjloader", "1.4.0", loadOBJ, loadPBin, savePBin);

	if (desc.initialVertices == VK_NULL_HANDLE || desc.initialIndices == VK_NULL_HANDLE)
		throw std::runtime_error("Failed to load model.");

	return desc;
}

} // namespace model

template <>
Model<GraphicsBackend::Vulkan>::Model(
	ModelDesc<GraphicsBackend::Vulkan>&& desc,
	CommandContext<GraphicsBackend::Vulkan>& commandContext)
: myModelDesc(std::move(desc))
, myBindings(model::calculateInputBindingDescriptions(myModelDesc.attributes))
, myVertexBuffer({
		myModelDesc.deviceContext,
		myModelDesc.vertexBufferSize,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		myModelDesc.initialVertices,
		myModelDesc.initialVerticesMemory,
		myModelDesc.debugName + "_vertices"},
	commandContext)
, myIndexBuffer({
		myModelDesc.deviceContext,
		myModelDesc.indexBufferSize,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		myModelDesc.initialIndices,
		myModelDesc.initialIndicesMemory,
		myModelDesc.debugName + "_indices"},
	commandContext)
{
}

template <>
Model<GraphicsBackend::Vulkan>::Model(
	const std::filesystem::path& modelFile,
	CommandContext<GraphicsBackend::Vulkan>& commandContext)
	: Model(model::load(modelFile, commandContext.getCommandContextDesc().deviceContext), commandContext)
{
}

template <>
void Model<GraphicsBackend::Vulkan>::deleteInitialData()
{
	myVertexBuffer.deleteInitialData();
	myIndexBuffer.deleteInitialData();
}
