#pragma once

#include "aabb.h"
#include "buffer.h"
#include "device.h"
#include "vertex.h"

#include <filesystem>
#include <string>
#include <vector>


template <GraphicsBackend B>
struct ModelCreateDesc
{
	std::shared_ptr<DeviceContext<B>> deviceContext;
	AABB3f aabb = {};
	// todo: reconsider.
	std::vector<SerializableVertexInputAttributeDescription<B>> attributes;
	//
	DeviceSize<B> indexBufferSize = 0;
	DeviceSize<B> vertexBufferSize = 0;
	uint32_t indexCount = 0;
	// temp: these will be destroyed when calling deleteInitialData()
	BufferHandle<B> initialData = 0;
	AllocationHandle<B> initialMemory = 0;
    //
	// todo: reconsider.
	std::string debugName;
	//
};

template <GraphicsBackend B>
class Model
{
public:

	Model(Model&& other) = default;
	Model(ModelCreateDesc<B>&& desc, CommandContext<B>& commandContext);
	Model(const std::filesystem::path& modelFile, CommandContext<B>& commandContext);

	const auto& getDesc() const { return myDesc; }
	const auto& getBuffer() const { return myBuffer; }
	const auto& getBindings() const { return myBindings; }
	constexpr auto getIndexOffset() const { return 0; }
	const auto& getVertexOffset() const { return myDesc.indexBufferSize; }

private:

	void deleteInitialData();

	ModelCreateDesc<B> myDesc = {};
	Buffer<B> myBuffer = {};
	std::vector<VertexInputBindingDescription<B>> myBindings;
};
