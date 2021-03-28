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
struct DescriptorSetLayoutCreateDesc
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
class DescriptorSetLayout : public DeviceObject<B>
{
public:

    using ShaderVariableBindingsMap = UnorderedMap<
        uint64_t,
        std::tuple<uint32_t, DescriptorType<B>, uint32_t>,
        IdentityHash<uint64_t>>;

    constexpr DescriptorSetLayout() noexcept = default;
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
    const auto& getShaderVariableBindings() const { return std::get<2>(myLayout); }
    const auto& getShaderVariableBinding(uint64_t shaderVariableNameHash) const { return std::get<2>(myLayout).at(shaderVariableNameHash); }

private:

    using ValueType = std::tuple<
        DescriptorSetLayoutHandle<B>,
        SamplerVector<B>,
        ShaderVariableBindingsMap>;

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
struct DescriptorSetArrayCreateDesc
{
    DescriptorPoolHandle<B> pool = {};
};

template <GraphicsBackend B>
class DescriptorSetArray : public DeviceObject<B>
{
    static constexpr size_t kDescriptorSetCount = 16;
    using ArrayType = std::array<DescriptorSetHandle<B>, kDescriptorSetCount>;

public:

    constexpr DescriptorSetArray() noexcept = default;
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

template <GraphicsBackend B>
struct DescriptorUpdateTemplateCreateDesc
{
    DescriptorUpdateTemplateType<B> templateType = {};
    DescriptorSetLayoutHandle<B> descriptorSetLayout = {};
    PipelineBindPoint<B> pipelineBindPoint = {};
    PipelineLayoutHandle<B> pipelineLayout = {};
    uint32_t set = 0ul;
};

template <GraphicsBackend B>
class DescriptorUpdateTemplate : public DeviceObject<B>
{
public:

    constexpr DescriptorUpdateTemplate() noexcept = default;
    DescriptorUpdateTemplate(DescriptorUpdateTemplate&& other) noexcept;
    DescriptorUpdateTemplate(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorUpdateTemplateCreateDesc<B>&& desc);
    ~DescriptorUpdateTemplate();

    DescriptorUpdateTemplate& operator=(DescriptorUpdateTemplate&& other) noexcept;
    operator auto() const noexcept { return myHandle; }
    bool operator==(const DescriptorUpdateTemplate& other) const noexcept { return myHandle == other.myHandle; }

    void swap(DescriptorUpdateTemplate& rhs) noexcept;
    friend void swap(DescriptorUpdateTemplate& lhs, DescriptorUpdateTemplate& rhs) noexcept { lhs.swap(rhs); }

    const auto& getDesc() const noexcept { return myDesc; }
    const auto& getEntries() const noexcept { return myEntries; }

    void setEntries(std::vector<DescriptorUpdateTemplateEntry<B>>&& entries);

private:

    DescriptorUpdateTemplateHandle<B> internalCreateTemplate();
    void internalDestroyTemplate();

    DescriptorUpdateTemplate( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorUpdateTemplateCreateDesc<B>&& desc,
        DescriptorUpdateTemplateHandle<B>&& handle);

    DescriptorUpdateTemplateCreateDesc<B> myDesc = {};
    std::vector<DescriptorUpdateTemplateEntry<B>> myEntries;
    DescriptorUpdateTemplateHandle<B> myHandle = {};
};

template <GraphicsBackend B>
using BindingVariant = std::variant<
    DescriptorBufferInfo<B>,
    DescriptorImageInfo<B>,
    BufferViewHandle<B>,
    AccelerationStructureHandle<B>,
    std::tuple<const void*, uint32_t>>; // InlineUniformBlock 

template <GraphicsBackend B>
using BindingValue = std::tuple<
    uint32_t, // offset
    uint32_t, // count
    DescriptorType<B>, // type
    RangeSet<uint32_t>>; // array ranges

template <GraphicsBackend B>
using BindingsMap = FlatMap<uint32_t, BindingValue<B>>;

template <GraphicsBackend B>
using BindingsData = std::vector<BindingVariant<B>>;

enum class DescriptorSetStatus : uint8_t { Dirty, Ready };

template <GraphicsBackend B>
using DescriptorSetState = std::tuple<
    UpgradableSharedMutex<>,
    DescriptorSetStatus,
    BindingsMap<B>,
    BindingsData<B>,
    DescriptorUpdateTemplate<B>,
    std::optional<DescriptorSetArrayList<B>>>; // if std::nullopt -> uses push descriptors

#include "descriptorset.inl"
#include "descriptorset-vulkan.inl"
