#pragma once

#include "device.h"
#include "sampler.h"
#include "types.h"

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
class DescriptorSetVector : public DeviceResource<B>
{
public:

    DescriptorSetVector(DescriptorSetVector&& other);
    DescriptorSetVector( // allocates vector of descriptor set handles using vector of layouts
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::vector<DescriptorSetLayout<B>>& layouts,
        DescriptorSetVectorCreateDesc<B>&& desc);
    DescriptorSetVector( // takes ownership of provided descriptor set handles.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetVectorCreateDesc<B>&& desc,
        std::vector<DescriptorSetHandle<B>>&& descriptorSetHandles);
    ~DescriptorSetVector();

    DescriptorSetVector& operator=(DescriptorSetVector&& other);
    const auto& operator[](uint32_t set) const { return myDescriptorSets[set]; };

    const auto& getDesc() const { return myDesc; }

    auto size() const { return myDescriptorSets.size(); }
    auto data() const { return myDescriptorSets.data(); }

private:

    DescriptorSetVectorCreateDesc<B> myDesc = {};
	std::vector<DescriptorSetHandle<B>> myDescriptorSets;
};

#include "descriptorset.inl"
#include "descriptorset-vulkan.inl"
