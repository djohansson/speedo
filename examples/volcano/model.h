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
struct ModelCreateDesc
{
	// todo: avoid temp copy - copy directly from mapped memory to gpu
	VertexAllocator vertices;
	std::vector<uint32_t> indices;
	std::vector<SerializableVertexInputAttributeDescription<B>> attributes;
	AABB3f aabb = {};
	std::string debugName;
};

template <GraphicsBackend B>
class Model
{
public:

	Model(DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
		ModelCreateDesc<B>&& modelData);

	Model(DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
		const std::filesystem::path& modelFile);

	~Model();

	const auto getVertexBuffer() const { return myVertexBuffer; }
	const auto getVertexBufferMemory() const { return myVertexBufferMemory; }
	const auto getIndexBuffer() const { return myIndexBuffer; }
	const auto getIndexBufferMemory() const { return myIndexBufferMemory; }
	
	const auto getIndexCount() const { return myIndexCount; }

	const auto& getAttributes() const { return myAttributes; }
	const auto& getBindings() const { return myBindings; }

	const auto& getAABB() const { return myAABB; }

private:

	AllocatorHandle<B> myAllocator = 0;

	BufferHandle<B> myVertexBuffer = 0;
	AllocationHandle<B> myVertexBufferMemory = 0;
	BufferHandle<B> myIndexBuffer = 0;
	AllocationHandle<B> myIndexBufferMemory = 0;
	
	uint32_t myIndexCount = 0;

	std::vector<VertexInputAttributeDescription<B>> myAttributes;
	std::vector<VertexInputBindingDescription<B>> myBindings;

	AABB3f myAABB = {};
};
