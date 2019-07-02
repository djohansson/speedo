#include "model.h"

namespace model
{

std::vector<VertexInputBindingDescription<GraphicsBackend::Vulkan>>
calculateInputBindingDescriptions(
	const std::vector<SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>>&
		attributeDescriptions)
{
	using AttributeMap = std::map<uint32_t, std::pair<VkFormat, uint32_t>>;

	AttributeMap attributes;

	for (const auto& attribute : attributeDescriptions)
	{
		assert(attribute.binding == 0); // todo: please implement me

		attributes[attribute.location] = std::make_pair(attribute.format, attribute.offset);
	}

	//int32_t lastBinding = -1;
	int32_t lastLocation = -1;
	uint32_t lastOffset = 0;
	uint32_t lastSize = 0;

	uint32_t stride = 0;

	for (const auto& [location, formatAndOffset] : attributes)
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

} // namespace model

template <>
Model<GraphicsBackend::Vulkan>::Model(
    DeviceHandle<GraphicsBackend::Vulkan> device,
    CommandPoolHandle<GraphicsBackend::Vulkan> commandPool,
    QueueHandle<GraphicsBackend::Vulkan> queue,
    AllocatorHandle<GraphicsBackend::Vulkan> allocator,
    const std::byte* vertices, size_t verticesSizeBytes,
    const std::byte* indices, uint32_t indexCount, size_t indexSizeBytes,
    const std::vector<SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>>& attributeDescriptions,
    const char* filename)
: allocator(allocator)
, indexCount(indexCount)
{
    createDeviceLocalBuffer(
        device, commandPool, queue, allocator,
        vertices, verticesSizeBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        vertexBuffer, vertexBufferMemory, filename);

    createDeviceLocalBuffer(
        device, commandPool, queue, allocator,
        indices, indexCount * indexSizeBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        indexBuffer, indexBufferMemory, filename);

    attributes.reserve(attributeDescriptions.size());
    std::copy(
        attributeDescriptions.begin(), attributeDescriptions.end(),
        std::back_inserter(attributes));
    
    bindings = model::calculateInputBindingDescriptions(attributeDescriptions);
}

template <>
Model<GraphicsBackend::Vulkan>::~Model()
{
    vmaDestroyBuffer(allocator, vertexBuffer, vertexBufferMemory);
    vmaDestroyBuffer(allocator, indexBuffer, indexBufferMemory);
}
