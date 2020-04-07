#pragma once

#include "aabb.h"
#include "buffer.h"
#include "utils.h"
#include "vertex.h"

#include <filesystem>
#include <string>
#include <vector>


template <GraphicsBackend B>
struct ModelCreateDesc
{
	DeviceHandle<B> device = 0;
	AllocatorHandle<B> allocator = 0;
	AABB3f aabb = {};
	std::vector<SerializableVertexInputAttributeDescription<B>> attributes;
	DeviceSize<B> vertexBufferSize = 0;
	DeviceSize<B> indexBufferSize = 0;
	uint32_t indexCount = 0;
	// these will be destroyed when calling deleteInitialData()
	BufferHandle<B> initialVertices = 0;
	AllocationHandle<B> initialVerticesMemory = 0;
	BufferHandle<B> initialIndices = 0;
	AllocationHandle<B> initialIndicesMemory = 0;
    //
	std::string debugName;
};

template <GraphicsBackend B>
class Model : Noncopyable
{
public:

	Model(ModelCreateDesc<B>&& desc, CommandBufferHandle<B> commandBuffer);
	Model(const std::filesystem::path& modelFile,
		DeviceHandle<B> device, AllocatorHandle<B> allocator, CommandBufferHandle<B> commandBuffer);

	void deleteInitialData();

	const auto& getDesc() const { return myDesc; }
	const auto& getVertexBuffer() const { return myVertexBuffer; }
	const auto& getIndexBuffer() const { return myIndexBuffer; }
	const auto& getBindings() const { return myBindings; }

private:

	ModelCreateDesc<B> myDesc = {};
	std::vector<VertexInputBindingDescription<B>> myBindings;
	Buffer<B> myVertexBuffer = {};
	Buffer<B> myIndexBuffer = {};
};
