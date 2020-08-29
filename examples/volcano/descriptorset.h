#pragma once

#include "device.h"
#include "types.h"

#include <map>
#include <memory>
#include <variant>
#include <vector>

template <GraphicsBackend B>
struct SerializableDescriptorSetLayoutBinding : public DescriptorSetLayoutBinding<B>
{
	using BaseType = DescriptorSetLayoutBinding<B>;
};

template <GraphicsBackend B>
using DescriptorSetLayoutBindingsMap = std::map<uint32_t, std::vector<SerializableDescriptorSetLayoutBinding<B>>>; // set, bindings

template <GraphicsBackend B>
class DescriptorSetLayout : public DeviceResource<B>
{
public:

    DescriptorSetLayout(DescriptorSetLayout&& other);
    DescriptorSetLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::vector<SerializableDescriptorSetLayoutBinding<B>>& bindings);
    DescriptorSetLayout( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutHandle<B>&& descriptorSetLayout);
    ~DescriptorSetLayout();

    DescriptorSetLayout& operator=(DescriptorSetLayout&& other);
    operator auto() const { return myDescriptorSetLayout; }

private:

	DescriptorSetLayoutHandle<B> myDescriptorSetLayout = {};
};

template <GraphicsBackend B>
class DescriptorSetVector : public DeviceResource<B>
{
public:

    DescriptorSetVector(DescriptorSetVector&& other);
    DescriptorSetVector(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::vector<DescriptorSetLayout<Vk>>& layouts);
    DescriptorSetVector( // takes ownership of provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<DescriptorSetHandle<B>>&& descriptorSetVector);
    ~DescriptorSetVector();

    DescriptorSetVector& operator=(DescriptorSetVector&& other);
    DescriptorSetHandle<B> operator[](uint32_t set) const { return myDescriptorSets[set]; };

    auto size() const { return myDescriptorSets.size(); }
    auto data() const { return myDescriptorSets.data(); }

    template <typename T>
    void set(T&& data, uint32_t set, uint32_t binding, DescriptorType<B> type, uint32_t index = 0);

    void update();

private:

    using VariantType = std::variant<
        std::vector<DescriptorBufferInfo<B>>,
        std::vector<DescriptorImageInfo<B>>,
        std::vector<BufferViewHandle<B>>>;

	std::vector<DescriptorSetHandle<B>> myDescriptorSets;
    std::vector<WriteDescriptorSet<B>> myDescriptorWrites;
    std::map<uint32_t, std::vector<std::tuple<DescriptorType<B>, VariantType>>> myData;
};

#include "descriptorset-vulkan.inl"
