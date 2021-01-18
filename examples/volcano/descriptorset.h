#pragma once

#include "device.h"
#include "sampler.h"
#include "types.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

template <GraphicsBackend B>
struct DescriptorSetLayoutCreateDesc : DeviceResourceCreateDesc<B>
{
    std::vector<DescriptorSetLayoutBinding<B>> bindings;
    std::vector<std::string> variableNames;
    std::vector<SamplerCreateInfo<B>> immutableSamplers;
    std::optional<PushConstantRange<B>> pushConstantRange;
    DescriptorSetLayoutCreateFlags<B> flags = {};
};

template <GraphicsBackend B>
class DescriptorSetLayout : public DeviceResource<B>
{
    using ValueType = std::tuple<DescriptorSetLayoutHandle<B>, SamplerVector<B>>;

public:

    DescriptorSetLayout(DescriptorSetLayout&& other);
    DescriptorSetLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutCreateDesc<B>&& desc);
    ~DescriptorSetLayout();

    DescriptorSetLayout& operator=(DescriptorSetLayout&& other);
    operator auto() const { return std::get<0>(myLayout); }
    bool operator==(const DescriptorSetLayout& other) const { return myLayout == other; }
    bool operator<(const DescriptorSetLayout& other) const { return myLayout < other; }

    const auto& getDesc() const { return myDesc; }
    auto getKey() const { return myKey; }
    const auto& getImmutableSamplers() const { return std::get<1>(myLayout); }

private:

    DescriptorSetLayout( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutCreateDesc<B>&& desc,
        ValueType&& layout);

    DescriptorSetLayoutCreateDesc<B> myDesc = {};
    uint64_t myKey = 0;
	ValueType myLayout = {};
};

template <GraphicsBackend B>
using DescriptorSetLayoutMap = UnorderedMapType<uint32_t, DescriptorSetLayout<B>>; // [set, layout]

template <GraphicsBackend B>
struct DescriptorSetArrayCreateDesc : DeviceResourceCreateDesc<B>
{
    DescriptorPoolHandle<B> pool = {};
};

template <GraphicsBackend B>
class DescriptorSetArray : public DeviceResource<B>
{   
public:

    static constexpr size_t kDescriptorSetCount = 256;

    DescriptorSetArray(DescriptorSetArray&& other);
    DescriptorSetArray( // allocates array of descriptor set handles using single layout
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        const DescriptorSetLayout<Vk>& layout,
        DescriptorSetArrayCreateDesc<Vk>&& desc);
    ~DescriptorSetArray();

    DescriptorSetArray& operator=(DescriptorSetArray&& other);
    const auto& operator[](uint8_t index) const { return myDescriptorSets[index]; };

    const auto& getDesc() const { return myDesc; }

private:

    using ArrayType = std::array<DescriptorSetHandle<B>, kDescriptorSetCount>;

    DescriptorSetArray( // takes ownership of provided descriptor set handles
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        DescriptorSetArrayCreateDesc<Vk>&& desc,
        ArrayType&& descriptorSetHandles);

    DescriptorSetArrayCreateDesc<B> myDesc = {};
	ArrayType myDescriptorSets;
};

#include "descriptorset.inl"
#include "descriptorset-vulkan.inl"
