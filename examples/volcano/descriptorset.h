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
    DescriptorSetLayout( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutCreateDesc<B>&& desc,
        ValueType&& layout);
    ~DescriptorSetLayout();

    DescriptorSetLayout& operator=(DescriptorSetLayout&& other);
    operator auto() const { return std::get<0>(myLayout); }
    bool operator==(const DescriptorSetLayout& other) const { return myLayout == other; }
    bool operator<(const DescriptorSetLayout& other) const { return myLayout < other; }

    const auto& getDesc() const { return myDesc; }

private:

    DescriptorSetLayoutCreateDesc<B> myDesc = {};
	ValueType myLayout = {};
};

template <GraphicsBackend B>
using DescriptorSetLayoutMap = MapType<uint32_t, DescriptorSetLayout<B>>; // [set, layout]

template <GraphicsBackend B>
struct DescriptorSetArrayCreateDesc : DeviceResourceCreateDesc<B>
{
    DescriptorPoolHandle<B> pool = {};
};

template <GraphicsBackend B>
class DescriptorSetArray : public DeviceResource<B>
{
    static constexpr uint8_t kDescriptorSetCount = 4;
    using ArrayType = std::array<DescriptorSetHandle<B>, kDescriptorSetCount>;
    
public:

    DescriptorSetArray(DescriptorSetArray&& other);
    DescriptorSetArray( // allocates array of descriptor set handles using single layout
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        const DescriptorSetLayout<Vk>& layout,
        DescriptorSetArrayCreateDesc<Vk>&& desc);
    DescriptorSetArray( // takes ownership of provided descriptor set handles
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        DescriptorSetArrayCreateDesc<Vk>&& desc,
        ArrayType&& descriptorSetHandles);
    ~DescriptorSetArray();

    DescriptorSetArray& operator=(DescriptorSetArray&& other);
    auto operator[](uint8_t set) const { return myDescriptorSets[set]; };

    const auto& getDesc() const { return myDesc; }

private:

    DescriptorSetArrayCreateDesc<B> myDesc = {};
	ArrayType myDescriptorSets;
};

#include "descriptorset.inl"
#include "descriptorset-vulkan.inl"
