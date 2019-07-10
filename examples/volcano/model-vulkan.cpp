#include "model.h"

#include "aabb.h"
#include "file.h"
#include "vertex.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
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
	const std::vector<VkVertexInputAttributeDescription>& attributes)
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

ModelCreateDesc<GraphicsBackend::Vulkan> load(const std::filesystem::path& modelFile)
{
	ModelCreateDesc<GraphicsBackend::Vulkan> data = {};
	data.debugName = modelFile.u8string();
	
	auto loadPBin = [&data](std::istream& stream) {
		cereal::PortableBinaryInputArchive pbin(stream);
		// todo: avoid temp copy - copy directly from mapped memory to gpu
		pbin(data.vertices, data.indices, data.attributes, data.aabb);
	};

	auto savePBin = [&data](std::ostream& stream) {
		cereal::PortableBinaryOutputArchive pbin(stream);
		pbin(data.vertices, data.indices, data.attributes, data.aabb);
	};

	auto loadOBJ = [&data](std::istream& stream) {
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
		data.attributes.emplace_back(
			SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>{{0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 0u}});
		data.attributes.emplace_back(
			SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>{{1u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 12u}});
		data.attributes.emplace_back(
			SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>{{2u, 0u, VK_FORMAT_R32G32_SFLOAT, 24u}});

		std::unordered_map<uint64_t, uint32_t> uniqueVertices;

		data.vertices.setStride(32);

		ScopedVertexAllocation vertexScope(data.vertices);
		data.vertices.reserve(indexCount / 3); // guesstimate
		data.indices.reserve(indexCount);

		size_t posOffset = data.attributes[0].offset;
		size_t colorOffset = data.attributes[1].offset;
		size_t texCoordOffset = data.attributes[2].offset;

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
					uniqueVertices[vertexHash] = static_cast<uint32_t>(data.vertices.size() - 1);
					data.aabb.merge(*pos);
				}
				else
				{
					vertexScope.freeVertices(&vertex);
				}

				data.indices.push_back(uniqueVertices[vertexHash]);
			}
		}
	};

	loadCachedSourceFile(
		modelFile, modelFile, "tinyobjloader", "1.4.0", loadOBJ, loadPBin, savePBin);

	if (data.vertices.empty() || data.indices.empty())
		throw std::runtime_error("Failed to load model.");

	return data;
}

} // namespace model

template <>
Model<GraphicsBackend::Vulkan>::Model(
    VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
    ModelCreateDesc<GraphicsBackend::Vulkan>&& desc)
: myAllocator(allocator)
, myIndexCount(desc.indices.size())
, myAABB(desc.aabb)
{
    std::tie(myVertexBuffer, myVertexBufferMemory) = createDeviceLocalBuffer(
        device, commandPool, queue, allocator,
        desc.vertices.data(), desc.vertices.sizeBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        desc.debugName.c_str());

    std::tie(myIndexBuffer, myIndexBufferMemory) = createDeviceLocalBuffer(
        device, commandPool, queue, allocator,
        reinterpret_cast<const std::byte*>(desc.indices.data()), desc.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        desc.debugName.c_str());

    myAttributes.reserve(desc.attributes.size());
    std::copy(
        desc.attributes.begin(), desc.attributes.end(),
        std::back_inserter(myAttributes));
    
    myBindings = model::calculateInputBindingDescriptions(myAttributes);
}

template <>
Model<GraphicsBackend::Vulkan>::Model(
	VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
	const std::filesystem::path& modelFile)
	: Model(device, commandPool, queue, allocator, model::load(modelFile))
{
}

template <>
Model<GraphicsBackend::Vulkan>::~Model()
{
    vmaDestroyBuffer(myAllocator, myVertexBuffer, myVertexBufferMemory);
    vmaDestroyBuffer(myAllocator, myIndexBuffer, myIndexBufferMemory);
}
