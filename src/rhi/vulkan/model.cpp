#include "../model.h"

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

//#define TINYOBJLOADER_USE_EXPERIMENTAL
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
#	define TINYOBJ_LOADER_OPT_IMPLEMENTATION
#	include <experimental/tinyobj_loader_opt.h>
#else
#	define TINYOBJLOADER_IMPLEMENTATION
#	include <tiny_obj_loader.h>
#endif

namespace model
{

std::vector<VkVertexInputBindingDescription> calculateInputBindingDescriptions(
	const std::vector<VertexInputAttributeDescription<Vk>>& attributes)
{
	using AttributeMap = std::map<uint32_t, std::tuple<VkFormat, uint32_t>>;

	AttributeMap attributeMap;

	for (const auto& attribute : attributes)
	{
		assert(attribute.binding == 0); // todo: please implement me

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

	// assert(VK_VERTEX_INPUT_RATE_VERTEX); // todo: please implement me

	return {VertexInputBindingDescription<Vk>{0u, stride, VK_VERTEX_INPUT_RATE_VERTEX}};
}

std::tuple<ModelCreateDesc<Vk>, BufferHandle<Vk>, AllocationHandle<Vk>> load(
	const std::filesystem::path& modelFile, const std::shared_ptr<Device<Vk>>& device)
{
	ZoneScopedN("model::load");

	std::tuple<ModelCreateDesc<Vk>, BufferHandle<Vk>, AllocationHandle<Vk>> descAndInitialData;

	auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;

	auto loadBin = [&descAndInitialData, &device](auto& in)
	{
		ZoneScopedN("model::loadBin");

		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
		in(desc).or_throw();

		size_t size = desc.indexBufferSize + desc.vertexBufferSize;

		auto [locBufferHandle, locMemoryHandle] = createBuffer(
			device->getAllocator(),
			size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			"todo_insert_proper_name");

		{
			ZoneScopedN("model::loadBin::buffers");

			void* data;
			VK_CHECK(vmaMapMemory(device->getAllocator(), locMemoryHandle, &data));
			in(std::span(static_cast<std::byte*>(data), size)).or_throw();
			vmaUnmapMemory(device->getAllocator(), locMemoryHandle);

			bufferHandle = locBufferHandle;
			memoryHandle = locMemoryHandle;
		}
	};

	auto saveBin = [&descAndInitialData, &device](auto& out)
	{
		ZoneScopedN("model::saveBin");

		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
		out(desc).or_throw();

		size_t size = desc.indexBufferSize + desc.vertexBufferSize;

		void* data;
		VK_CHECK(vmaMapMemory(device->getAllocator(), memoryHandle, &data));
		out(std::span(static_cast<std::byte*>(data), size)).or_throw();
		vmaUnmapMemory(device->getAllocator(), memoryHandle);
	};

	auto loadOBJ = [&descAndInitialData, &device, &modelFile](auto& in)
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
		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, modelFile.string().c_str()))
			throw std::runtime_error(err);
#endif

		uint32_t indexCount = 0;
		for (const auto& shape : shapes)
		{
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
			for (uint32_t faceOffset = shape.face_offset;
				 faceOffset < (shape.face_offset + shape.length);
				 faceOffset++)
				indexCount += attrib.face_num_verts[faceOffset];
#else
			indexCount += shape.mesh.indices.size();
#endif
		}

		uint32_t offset = 0;

		uint32_t posOffset = offset;

		if (!attrib.vertices.empty())
		{
			constexpr VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;

			desc.attributes.emplace_back(
				VertexInputAttributeDescription<Vk>{static_cast<uint32_t>(desc.attributes.size()), 0, format, offset});

			offset += getFormatSize(format);
		}

		size_t normalOffset = offset;

		if (!attrib.normals.empty())
		{
			constexpr VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;

			desc.attributes.emplace_back(
				VertexInputAttributeDescription<Vk>{static_cast<uint32_t>(desc.attributes.size()), 0, format, offset});

			offset += getFormatSize(format);
		}

		size_t texCoordOffset = offset;

		if (!attrib.texcoords.empty())
		{
			constexpr VkFormat format = VK_FORMAT_R32G32_SFLOAT;

			desc.attributes.emplace_back(
				VertexInputAttributeDescription<Vk>{static_cast<uint32_t>(desc.attributes.size()), 0, format, offset});

			offset += getFormatSize(format);
		}

		size_t colorOffset = offset;

		if (!attrib.colors.empty())
		{
			constexpr VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;

			desc.attributes.emplace_back(
				VertexInputAttributeDescription<Vk>{static_cast<uint32_t>(desc.attributes.size()), 0, format, offset});

			offset += getFormatSize(format);
		}

		UnorderedMap<uint64_t, uint32_t> uniqueVertices;

		VertexAllocator vertices;
		vertices.setStride(offset);

		std::vector<uint32_t> indices;

		ScopedVertexAllocation vertexScope(vertices);
		vertices.reserve(indexCount / 3); // guesstimate
		indices.reserve(indexCount);

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
				
				if (!attrib.vertices.empty())
					*vertex.dataAs<glm::vec3>(posOffset) = {
						attrib.vertices[3 * index.vertex_index + 0],
						attrib.vertices[3 * index.vertex_index + 1],
						attrib.vertices[3 * index.vertex_index + 2]};

				if (!attrib.normals.empty())
					*vertex.dataAs<glm::vec3>(normalOffset) = {
						attrib.normals[3 * index.normal_index + 0],
						attrib.normals[3 * index.normal_index + 1],
						attrib.normals[3 * index.normal_index + 2]};

				if (!attrib.texcoords.empty())
					*vertex.dataAs<glm::vec2>(texCoordOffset) = {
						attrib.texcoords[2 * index.texcoord_index + 0],
						1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};

				if (!attrib.colors.empty())
					*vertex.dataAs<glm::vec3>(colorOffset) = {
						attrib.colors[3 * index.vertex_index + 0],
						attrib.colors[3 * index.vertex_index + 1],
						attrib.colors[3 * index.vertex_index + 2]};

				uint64_t vertexIndex = vertex.hash();
				if (uniqueVertices.count(vertexIndex) == 0)
				{
					uniqueVertices[vertexIndex] = static_cast<uint32_t>(vertices.size() - 1);

					if (!attrib.vertices.empty())
						desc.aabb.merge(*vertex.dataAs<glm::vec3>(posOffset));
				}
				else
				{
					vertexScope.freeVertices(&vertex);
				}
				indices.push_back(uniqueVertices[vertexIndex]);
			}
		}

		desc.indexBufferSize = indices.size() * sizeof(uint32_t);
		desc.vertexBufferSize = vertices.sizeBytes();
		desc.indexCount = indices.size();

		auto [locBufferHandle, locMemoryHandle] = createBuffer(
			device->getAllocator(),
			desc.indexBufferSize + desc.vertexBufferSize,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			"todo_insert_proper_name");

		{
			ZoneScopedN("model::loadObj::buffers");

			void* data;
			VK_CHECK(vmaMapMemory(device->getAllocator(), locMemoryHandle, &data));
			memcpy(data, indices.data(), desc.indexBufferSize);
			memcpy(
				static_cast<std::byte*>(data) + desc.indexBufferSize,
				vertices.data(),
				desc.vertexBufferSize);
			vmaUnmapMemory(device->getAllocator(), locMemoryHandle);

			bufferHandle = locBufferHandle;
			memoryHandle = locMemoryHandle;
		}
	};

	static constexpr char loaderType[] = "tinyobjloader";
	static constexpr char loaderVersion[] = "2.0.1";
	loadCachedSourceFile<loaderType, loaderVersion>(modelFile, loadOBJ, loadBin, saveBin);

	if (!bufferHandle)
		throw std::runtime_error("Failed to load model.");

	return descAndInitialData;
}

} // namespace model

template <>
Model<Vk>::Model(
	const std::shared_ptr<Device<Vk>>& device,
	CommandPoolContext<Vk>& commandContext,
	std::tuple<ModelCreateDesc<Vk>, BufferHandle<Vk>, AllocationHandle<Vk>>&& descAndInitialData)
	: Buffer(
		  device,
		  commandContext,
		  std::make_tuple(
			  BufferCreateDesc<Vk>{
				  std::get<0>(descAndInitialData).indexBufferSize +
					  std::get<0>(descAndInitialData).vertexBufferSize,
				  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
			  std::get<1>(descAndInitialData),
			  std::get<2>(descAndInitialData)))
	, myDesc(std::forward<ModelCreateDesc<Vk>>(std::get<0>(descAndInitialData)))
	, myBindings(model::calculateInputBindingDescriptions(myDesc.attributes))
{}

template <>
Model<Vk>::Model(
	const std::shared_ptr<Device<Vk>>& device,
	CommandPoolContext<Vk>& commandContext,
	const std::filesystem::path& modelFile)
	: Model(device, commandContext, model::load(modelFile, device))
{}

template <>
void Model<Vk>::swap(Model& rhs) noexcept
{
	Buffer<Vk>::swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myBindings, rhs.myBindings);
}
