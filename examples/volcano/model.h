#pragma once

#include "aabb.h"
#include "buffer.h"
#include "gfx.h"
#include "vertex.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

template <GraphicsBackend B>
struct ModelData
{
	VertexAllocator vertices;
	std::vector<uint32_t> indices;
	std::vector<SerializableVertexInputAttributeDescription<B>> attributes;
	AABB3f aabb = {};
	std::string debugName;
};

template <GraphicsBackend B>
struct Model
{
	Model(DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
		const ModelData<B>& modelData);

	Model(DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
		const std::filesystem::path& modelFile);

	~Model();

	AllocatorHandle<B> allocator = 0;

	BufferHandle<B> vertexBuffer = 0;
	AllocationHandle<B> vertexBufferMemory = 0;
	BufferHandle<B> indexBuffer = 0;
	AllocationHandle<B> indexBufferMemory = 0;
	
	uint32_t indexCount = 0;

	std::vector<VertexInputAttributeDescription<B>> attributes;
	std::vector<VertexInputBindingDescription<B>> bindings;
};
