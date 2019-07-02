#pragma once

#include "gfx-types.h"

template <GraphicsBackend B>
struct Model
{
	Model(DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
		const std::byte* vertices, size_t verticesSizeBytes,
		const std::byte* indices, uint32_t indexCount, size_t indexSizeBytes,
		const std::vector<SerializableVertexInputAttributeDescription<B>>& attributeDescriptions,
		const char* filename);

	~Model();

	AllocatorHandle<B> allocator;

	BufferHandle<B> vertexBuffer = 0;
	AllocationHandle<B> vertexBufferMemory = 0;
	BufferHandle<B> indexBuffer = 0;
	AllocationHandle<B> indexBufferMemory = 0;
	uint32_t indexCount = 0;

	std::vector<VertexInputAttributeDescription<B>> attributes;
	std::vector<VertexInputBindingDescription<B>> bindings;
};
