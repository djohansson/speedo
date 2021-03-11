#pragma once

#include "device.h"
#include "sampler.h"
#include "types.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

template <GraphicsBackend B>
struct DescriptorSetLayoutCreateDesc : DeviceResourceCreateDesc<B>
{
    std::vector<DescriptorSetLayoutBinding<B>> bindings;
    std::vector<DescriptorBindingFlags<B>> bindingFlags;
    std::vector<std::string> variableNames;
    std::vector<uint64_t> variableNameHashes;
    std::vector<SamplerCreateInfo<B>> immutableSamplers;
    std::optional<PushConstantRange<B>> pushConstantRange;
    DescriptorSetLayoutCreateFlags<B> flags = {};
};

template <GraphicsBackend B>
using ShaderVariableBindingsMap = UnorderedMap<
    uint64_t,
    std::tuple<DescriptorType<B>, uint32_t>,
    IdentityHash<uint64_t>>;

template <GraphicsBackend B>
class DescriptorSetLayout : public DeviceResource<B>
{
public:

    constexpr DescriptorSetLayout() = default;
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
    const auto& getImmutableSamplers() const { return std::get<1>(myLayout); }
    const auto& getShaderVariableBindingsMap() const { return std::get<2>(myLayout); }

private:

    using ValueType = std::tuple<
        DescriptorSetLayoutHandle<B>,
        SamplerVector<B>,
        ShaderVariableBindingsMap<B>>;

    DescriptorSetLayout( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutCreateDesc<B>&& desc,
        ValueType&& layout);

    DescriptorSetLayoutCreateDesc<B> myDesc = {};
    ValueType myLayout = {};
};

template <GraphicsBackend B>
using DescriptorSetLayoutFlatMap = FlatMap<uint32_t, DescriptorSetLayout<B>>;

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

    constexpr DescriptorSetArray() = default;
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
    static constexpr auto capacity() { return kDescriptorSetCount; }

private:

    DescriptorSetArray( // takes ownership of provided descriptor set handles
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        DescriptorSetArrayCreateDesc<Vk>&& desc,
        ArrayType&& descriptorSetHandles);

    DescriptorSetArrayCreateDesc<B> myDesc = {};
	ArrayType myDescriptorSets;
};

template <GraphicsBackend B>
using DescriptorSetArrayList = std::list<
    std::tuple<
        DescriptorSetArray<B>, // descriptor set array
        uint8_t, // current array index. move out from here perhaps?
        CopyableAtomic<uint32_t>>>; // reference count.

enum class BindingFlags : uint32_t { IsArray = 1u << 31 };

template <GraphicsBackend B>
using BindingVariant = std::variant<
    DescriptorBufferInfo<B>,
    std::vector<DescriptorBufferInfo<B>>,
    DescriptorImageInfo<B>,
    std::vector<DescriptorImageInfo<B>>,
    BufferViewHandle<B>,
    std::vector<BufferViewHandle<B>>,
    InlineUniformBlock<B>>; // InlineUniformBlock can only have one array element per binding

template <GraphicsBackend B>
using BindingValue = std::tuple<DescriptorType<B>, BindingVariant<B>, RangeSet<uint32_t>>;

template <GraphicsBackend B>
using BindingsMap = UnorderedMap<uint32_t, BindingValue<B>>;

enum class DescriptorSetStatus : uint8_t { Dirty, Ready };

template <GraphicsBackend B>
using DescriptorSetState = std::tuple<
    BindingsMap<B>,
    UpgradableSharedMutex<uint8_t>,
    DescriptorSetStatus,
    DescriptorUpdateTemplateHandle<B>,
    std::optional<DescriptorSetArrayList<B>>>; // [bindings, mutex, set state, descriptorSets (optional - if std::nullopt -> uses push descriptors)]

#include "descriptorset.inl"
#include "descriptorset-vulkan.inl"
