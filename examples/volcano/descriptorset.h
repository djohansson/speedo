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
    std::vector<uint64_t> variableNameHashes;
    std::vector<SamplerCreateInfo<B>> immutableSamplers;
    std::optional<PushConstantRange<B>> pushConstantRange;
    DescriptorSetLayoutCreateFlags<B> flags = {};
};

template <GraphicsBackend B>
class DescriptorSetLayout : public DeviceResource<B>
{
    using BindingsMapType = UnorderedMapType<uint64_t, uint32_t, PassThroughHash<uint64_t>>;
    using ValueType = std::tuple<DescriptorSetLayoutHandle<B>, SamplerVector<B>, BindingsMapType>;

public:

    DescriptorSetLayout() = default;
    DescriptorSetLayout(DescriptorSetLayout&& other) noexcept;
    DescriptorSetLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutCreateDesc<B>&& desc);
    ~DescriptorSetLayout();

    DescriptorSetLayout& operator=(DescriptorSetLayout&& other) noexcept;
    operator auto() const { return std::get<0>(myLayout); }
    bool operator==(const DescriptorSetLayout& other) const { return myLayout == other; }
    bool operator<(const DescriptorSetLayout& other) const { return myLayout < other; }

    void swap(DescriptorSetLayout& rhs) noexcept;
    friend void swap(DescriptorSetLayout& lhs, DescriptorSetLayout& rhs) noexcept { lhs.swap(rhs); }

    const auto& getDesc() const { return myDesc; }
    auto getKey() const { return myKey; }
    const auto& getImmutableSamplers() const { return std::get<1>(myLayout); }
    const auto& getBindingsMap() const { return std::get<2>(myLayout); }

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
using DescriptorSetLayoutMapType = UnorderedMapType<uint32_t, DescriptorSetLayout<B>>; // [set, layout]

template <GraphicsBackend B>
struct DescriptorSetArrayCreateDesc : DeviceResourceCreateDesc<B>
{
    DescriptorPoolHandle<B> pool = {};
};

template <GraphicsBackend B>
class DescriptorSetArray : public DeviceResource<B>
{
    static constexpr size_t kDescriptorSetCount = 16;
    using ArrayType = std::array<DescriptorSetHandle<B>, kDescriptorSetCount>;

public:

    DescriptorSetArray() = default;
    DescriptorSetArray(DescriptorSetArray&& other) noexcept;
    DescriptorSetArray( // allocates array of descriptor set handles using single layout
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        const DescriptorSetLayout<Vk>& layout,
        DescriptorSetArrayCreateDesc<Vk>&& desc);
    ~DescriptorSetArray();

    DescriptorSetArray& operator=(DescriptorSetArray&& other) noexcept;
    const auto& operator[](uint8_t index) const { return myDescriptorSets[index]; };

    void swap(DescriptorSetArray& rhs) noexcept;
    friend void swap(DescriptorSetArray& lhs, DescriptorSetArray& rhs) noexcept { lhs.swap(rhs); }

    const auto& getDesc() const { return myDesc; }
    constexpr size_t getSize() const { return myDescriptorSets.size(); }

private:

    DescriptorSetArray( // takes ownership of provided descriptor set handles
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        DescriptorSetArrayCreateDesc<Vk>&& desc,
        ArrayType&& descriptorSetHandles);

    DescriptorSetArrayCreateDesc<B> myDesc = {};
	ArrayType myDescriptorSets;
};

#include "descriptorset.inl"
#include "descriptorset-vulkan.inl"
