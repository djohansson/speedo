#pragma once

#include "device.h"
#include "sampler.h"
#include "types.h"

#include <array>
#include <map>
#include <memory>
#include <variant>
#include <vector>

template <GraphicsBackend B>
using DescriptorSetLayoutMap = std::map<
    uint32_t, // set
    std::tuple<
        std::vector<DescriptorSetLayoutBinding<B>>, // bindings
        std::vector<SamplerCreateInfo<B>>, // immutable samplers
        DescriptorSetLayoutCreateFlags<B>>>; // layout flags

template <GraphicsBackend B>
struct DescriptorSetLayoutCreateDesc : DeviceResourceCreateDesc<B>
{
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
        DescriptorSetLayoutCreateDesc<B>&& desc,
        const std::vector<SamplerCreateInfo<B>>& immutableSamplers,
        std::vector<DescriptorSetLayoutBinding<B>>& bindings);
    DescriptorSetLayout( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutCreateDesc<B>&& desc,
        ValueType&& layout);
    ~DescriptorSetLayout();

    DescriptorSetLayout& operator=(DescriptorSetLayout&& other);
    operator auto() const { return std::get<0>(myLayout); }

    const auto& getDesc() const { return myDesc; }

private:

    DescriptorSetLayoutCreateDesc<B> myDesc = {};
	ValueType myLayout = {};
};

template <GraphicsBackend B>
struct DescriptorSetVectorCreateDesc : DeviceResourceCreateDesc<B>
{
    DescriptorPoolHandle<B> pool = {};
};

template <GraphicsBackend B>
class DescriptorSetArray : public DeviceResource<B>
{
public:

    static constexpr uint32_t kDescriptorSetCount = 4;
    using ArrayType = std::array<DescriptorSetHandle<B>, kDescriptorSetCount>;

    DescriptorSetArray(DescriptorSetArray&& other);
    DescriptorSetArray( // allocates array of descriptor set handles using single layout
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        const DescriptorSetLayout<Vk>& layout,
        DescriptorSetVectorCreateDesc<Vk>&& desc);
    DescriptorSetArray( // takes ownership of provided descriptor set handles
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        DescriptorSetVectorCreateDesc<Vk>&& desc,
        ArrayType&& descriptorSetHandles);
    ~DescriptorSetArray();

    DescriptorSetArray& operator=(DescriptorSetArray&& other);

    constexpr auto& operator[](uint32_t set) const { return myDescriptorSets[set]; };
    operator const auto&() const { return myDescriptorSets; }

    const auto& getDesc() const { return myDesc; }

private:

    DescriptorSetVectorCreateDesc<B> myDesc = {};
	ArrayType myDescriptorSets;
};

#include "descriptorset.inl"
#include "descriptorset-vulkan.inl"
