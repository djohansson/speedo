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
    using LayoutType = std::tuple<DescriptorSetLayoutHandle<B>, SamplerVector<B>>;

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
        LayoutType&& layout);
    ~DescriptorSetLayout();

    DescriptorSetLayout& operator=(DescriptorSetLayout&& other);
    operator auto() const { return std::get<0>(myLayout); }

    const auto& getDesc() const { return myDesc; }

private:

    DescriptorSetLayoutCreateDesc<B> myDesc = {};
	LayoutType myLayout = {};
};

template <GraphicsBackend B>
struct DescriptorSetVectorCreateDesc : DeviceResourceCreateDesc<B>
{
    DescriptorPoolHandle<B> pool = {};
    std::vector<DescriptorSetLayoutHandle<B>> layouts;
};

template <GraphicsBackend B>
class DescriptorSetVector : public DeviceResource<B>
{
public:

    DescriptorSetVector(const DescriptorSetVector& other);
    DescriptorSetVector(DescriptorSetVector&& other);
    DescriptorSetVector( // allocates vector of descriptor set handles using vector of layout handles in desc
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetVectorCreateDesc<B>&& desc);
    DescriptorSetVector( // takes ownership of provided descriptor set handles.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetVectorCreateDesc<B>&& desc,
        std::vector<DescriptorSetHandle<B>>&& descriptorSetHandles);
    ~DescriptorSetVector();

    DescriptorSetVector& operator=(const DescriptorSetVector& other);
    DescriptorSetVector& operator=(DescriptorSetVector&& other);
    auto operator[](uint32_t set) const { return myDescriptorSets[set]; };

    const auto& getDesc() const { return myDesc; }

    auto size() const { return myDescriptorSets.size(); }
    auto data() const { return myDescriptorSets.data(); }

    template <typename T>
    void set(T&& data, DescriptorType<B> type, uint32_t set, uint32_t binding, uint32_t index = 0);
    
    void copy(DescriptorSetVector<B>& dst) const;
    void push(
        CommandBufferHandle<B> cmd,
        PipelineBindPoint<B> bindPoint,
        PipelineLayoutHandle<B> layout,
        uint32_t set,
        uint32_t bindingOffset,
        uint32_t bindingCount) const;
    void write() const;

private:

    using VariantType = std::variant<
        std::vector<DescriptorBufferInfo<B>>,
        std::vector<DescriptorImageInfo<B>>,
        std::vector<BufferViewHandle<B>>>;

    DescriptorSetVectorCreateDesc<B> myDesc = {};
	std::vector<DescriptorSetHandle<B>> myDescriptorSets;
    std::map<uint32_t, std::vector<std::tuple<DescriptorType<B>, VariantType>>> myData;
};

#include "descriptorset.inl"
#include "descriptorset-vulkan.inl"
