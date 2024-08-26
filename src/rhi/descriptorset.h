#pragma once

#include "device.h"
#include "sampler.h"
#include "types.h"

#include <core/upgradablesharedmutex.h>

#include <array>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

template <GraphicsApi G>
struct DescriptorSetLayoutCreateDesc
{
	std::vector<DescriptorSetLayoutBinding<G>> bindings;
	std::vector<DescriptorBindingFlags<G>> bindingFlags;
	std::vector<std::string> variableNames;
	std::vector<uint64_t> variableNameHashes;
	std::vector<SamplerCreateInfo<G>> immutableSamplers;
	std::optional<PushConstantRange<G>> pushConstantRange;
	DescriptorSetLayoutCreateFlags<G> flags{};
};

template <GraphicsApi G>
class DescriptorSetLayout final : public DeviceObject<G>
{
public:
	using ShaderVariableBindingsMap = UnorderedMap<
		uint64_t,
		std::tuple<uint32_t, DescriptorType<G>, uint32_t>,
		IdentityHash<uint64_t>>;

	constexpr DescriptorSetLayout() noexcept = default;
	DescriptorSetLayout(DescriptorSetLayout&& other) noexcept;
	DescriptorSetLayout(
		const std::shared_ptr<Device<G>>& device,
		DescriptorSetLayoutCreateDesc<G>&& desc);
	~DescriptorSetLayout() override;

	DescriptorSetLayout& operator=(DescriptorSetLayout&& other) noexcept;
	[[nodiscard]] operator auto() const { return std::get<0>(myLayout); }//NOLINT(google-explicit-constructor)
	[[nodiscard]] bool operator==(const DescriptorSetLayout& other) const { return myLayout == other; }
	[[nodiscard]] bool operator<(const DescriptorSetLayout& other) const { return myLayout < other; }

	void Swap(DescriptorSetLayout& rhs) noexcept;
	friend void Swap(DescriptorSetLayout& lhs, DescriptorSetLayout& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] const auto& GetImmutableSamplers() const noexcept { return std::get<1>(myLayout); }
	[[nodiscard]] const auto& GetShaderVariableBindings() const noexcept { return std::get<2>(myLayout); }
	[[nodiscard]] const auto& GetShaderVariableBinding(uint64_t shaderVariableNameHash) const
	{
		return std::get<2>(myLayout).at(shaderVariableNameHash);
	}

private:
	using ValueType =
		std::tuple<DescriptorSetLayoutHandle<G>, SamplerVector<G>, ShaderVariableBindingsMap>;

	DescriptorSetLayout( // takes ownership of provided handle
		const std::shared_ptr<Device<G>>& device,
		DescriptorSetLayoutCreateDesc<G>&& desc,
		ValueType&& layout);

	DescriptorSetLayoutCreateDesc<G> myDesc{};
	ValueType myLayout{};
};

template <GraphicsApi G>
using DescriptorSetLayoutFlatMap = FlatMap<uint32_t, DescriptorSetLayout<G>>;

template <GraphicsApi G>
struct DescriptorSetArrayCreateDesc
{
	DescriptorPoolHandle<G> pool{};
};

template <GraphicsApi G>
class DescriptorSetArray final : public DeviceObject<G>
{
	static constexpr size_t kDescriptorSetCount = 16;
	using ArrayType = std::array<DescriptorSetHandle<G>, kDescriptorSetCount>;

public:
	constexpr DescriptorSetArray() noexcept = default;
	DescriptorSetArray(DescriptorSetArray&& other) noexcept;
	DescriptorSetArray( // allocates array of descriptor set handles using single layout
		const std::shared_ptr<Device<G>>& device,
		const DescriptorSetLayout<G>& layout,
		DescriptorSetArrayCreateDesc<G>&& desc);
	~DescriptorSetArray() override;

	DescriptorSetArray& operator=(DescriptorSetArray&& other) noexcept;
	[[nodiscard]] const auto& operator[](uint8_t index) const { return myDescriptorSets[index]; };

	void Swap(DescriptorSetArray& rhs) noexcept;
	friend void Swap(DescriptorSetArray& lhs, DescriptorSetArray& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }

	[[nodiscard]] static constexpr auto Capacity() { return kDescriptorSetCount; }

private:
	DescriptorSetArray( // takes ownership of provided descriptor set handles
		const std::shared_ptr<Device<G>>& device,
		DescriptorSetArrayCreateDesc<G>&& desc,
		ArrayType&& descriptorSetHandles);

	DescriptorSetArrayCreateDesc<G> myDesc{};
	ArrayType myDescriptorSets;
};

template <GraphicsApi G>
using DescriptorSetArrayList = std::list<std::tuple<DescriptorSetArray<G>, uint8_t>>;

template <GraphicsApi G>
struct DescriptorUpdateTemplateCreateDesc
{
	DescriptorUpdateTemplateType<G> templateType{};
	DescriptorSetLayoutHandle<G> descriptorSetLayout{};
	PipelineBindPoint<G> pipelineBindPoint{};
	PipelineLayoutHandle<G> pipelineLayout{};
	uint32_t set = 0UL;
};

template <GraphicsApi G>
class DescriptorUpdateTemplate final : public DeviceObject<G>
{
public:
	constexpr DescriptorUpdateTemplate() noexcept = default;
	DescriptorUpdateTemplate(DescriptorUpdateTemplate&& other) noexcept;
	DescriptorUpdateTemplate(
		const std::shared_ptr<Device<G>>& device,
		DescriptorUpdateTemplateCreateDesc<G>&& desc);
	~DescriptorUpdateTemplate() override;

	DescriptorUpdateTemplate& operator=(DescriptorUpdateTemplate&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myHandle; }//NOLINT(google-explicit-constructor)
	[[nodiscard]] bool operator==(const DescriptorUpdateTemplate& other) const noexcept
	{
		return myHandle == other.myHandle;
	}

	void Swap(DescriptorUpdateTemplate& rhs) noexcept;
	friend void Swap(DescriptorUpdateTemplate& lhs, DescriptorUpdateTemplate& rhs) noexcept
	{
		lhs.Swap(rhs);
	}

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] const auto& GetEntries() const noexcept { return myEntries; }

	void SetEntries(std::vector<DescriptorUpdateTemplateEntry<G>>&& entries);

private:
	void InternalDestroyTemplate();
	DescriptorUpdateTemplate( // takes ownership of provided handle
		const std::shared_ptr<Device<G>>& device,
		DescriptorUpdateTemplateCreateDesc<G>&& desc,
		DescriptorUpdateTemplateHandle<G>&& handle);

	DescriptorUpdateTemplateCreateDesc<G> myDesc{};
	std::vector<DescriptorUpdateTemplateEntry<G>> myEntries;
	DescriptorUpdateTemplateHandle<G> myHandle{};
};

template <GraphicsApi G>
using BindingVariant = std::variant<
	DescriptorBufferInfo<G>,
	DescriptorImageInfo<G>,
	BufferViewHandle<G>,
	AccelerationStructureHandle<G>,
	std::tuple<const void*, uint32_t>>; // InlineUniformBlock

template <GraphicsApi G>
using BindingValue = std::tuple<
	uint32_t,			 // offset
	uint32_t,			 // count
	DescriptorType<G>,	 // type
	RangeSet<uint32_t>>; // array ranges

template <GraphicsApi G>
using BindingsMap = FlatMap<uint32_t, BindingValue<G>>;

template <GraphicsApi G>
using BindingsData = std::vector<BindingVariant<G>>;

enum class DescriptorSetStatus : uint8_t
{
	kDirty,
	kReady
};

template <GraphicsApi G>
using DescriptorSetState = std::tuple<
	UpgradableSharedMutex,
	DescriptorSetStatus,
	BindingsMap<G>,
	BindingsData<G>,
	DescriptorUpdateTemplate<G>,
	std::optional<DescriptorSetArrayList<G>>>; // if std::nullopt -> uses push descriptors

#include "descriptorset.inl"
