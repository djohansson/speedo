#pragma once

#include <rhi/deviceobject.h>
#include <rhi/sampler.h>
#include <rhi/types.h>

#include <core/upgradablesharedmutex.h>

#include <array>
#include <flat_map>
#include <list>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
struct DescriptorSetLayoutCreateDesc final : DeviceObjectCreateDesc<G>
{
	std::vector<DescriptorSetLayoutBinding<G>> bindings;
	std::vector<DescriptorBindingFlags<G>> bindingFlags;
	std::vector<std::string> variableNames;
	std::vector<uint64_t> variableNameHashes;
	std::vector<SamplerCreateInfo<G>> immutableSamplers;
	std::optional<PushConstantRange<G>> pushConstantRange;
	DescriptorSetLayoutCreateFlags<G> flags{};

	// see DeviceObjectCreateDesc::serialize for why this is needed
	constexpr static auto serialize(auto& archive, auto& self)//NOLINT(readability-identifier-naming)
	{
		using SelfType = std::remove_reference_t<decltype(self)>;
		using BaseType = std::conditional_t<std::is_const_v<SelfType>, const DeviceObjectCreateDesc<G>, DeviceObjectCreateDesc<G>>;
		return archive(
			static_cast<BaseType&>(self),
			self.bindings,
			self.bindingFlags,
			self.variableNames,
			self.variableNameHashes,
			self.immutableSamplers,
			self.pushConstantRange,
			self.flags);
	}
};

template <GraphicsApi G>
class DescriptorSetLayout;

template <GraphicsApi G>
struct ObjectTraits<DescriptorSetLayout<G>>
{
	using CreateDescType = DescriptorSetLayoutCreateDesc<G>;
};

template <GraphicsApi G>
class DescriptorSetLayout final : public DeviceObject<DescriptorSetLayout<G>>
{
public:
	using SuperType = DeviceObject<DescriptorSetLayout<G>>;
	using CreateDescType = ObjectTraits<DescriptorSetLayout<G>>::CreateDescType;

	constexpr DescriptorSetLayout() noexcept = default;
	DescriptorSetLayout(DescriptorSetLayout&& other) noexcept;
	explicit DescriptorSetLayout(CreateDescType&& desc);
	~DescriptorSetLayout();

	[[maybe_unused]] DescriptorSetLayout& operator=(DescriptorSetLayout&& other) noexcept;
	[[nodiscard]] operator auto() const { return std::get<0>(myLayout); }//NOLINT(google-explicit-constructor)
	[[nodiscard]] bool operator==(const DescriptorSetLayout& other) const { return myLayout == other; }
	[[nodiscard]] bool operator<(const DescriptorSetLayout& other) const { return myLayout < other; }

	void Swap(DescriptorSetLayout& rhs) noexcept;
	friend void Swap(DescriptorSetLayout& lhs, DescriptorSetLayout& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetImmutableSamplers() const noexcept { return std::get<1>(myLayout); }
	[[nodiscard]] const auto& GetShaderVariableBindings() const noexcept { return std::get<2>(myLayout); }
	[[nodiscard]] const auto& GetShaderVariableBinding(uint64_t shaderVariableNameHash) const
	{
		return std::get<2>(myLayout).at(shaderVariableNameHash);
	}

private:
	using ShaderVariableBindingsMap = core::UnorderedMap<
		uint64_t,
		std::tuple<uint32_t, DescriptorType<G>, uint32_t>,
		core::IdentityHash<uint64_t>>;
	using ValueType =
		std::tuple<DescriptorSetLayoutHandle<G>, SamplerVector<G>, ShaderVariableBindingsMap>;

	DescriptorSetLayout( // takes ownership of provided handle
		CreateDescType&& desc,
		ValueType&& layout);

	ValueType myLayout{};
};

template <GraphicsApi G>
using DescriptorSetLayoutFlatMap = std::flat_map<uint32_t, DescriptorSetLayout<G>>;

template <GraphicsApi G>
struct DescriptorSetArrayCreateDesc final : DeviceObjectCreateDesc<G>
{
	DescriptorPoolHandle<G> pool{};
};

template <GraphicsApi G>
class DescriptorSetArray;

template <GraphicsApi G>
struct ObjectTraits<DescriptorSetArray<G>>
{
	using CreateDescType = DescriptorSetArrayCreateDesc<G>;
};

template <GraphicsApi G>
class DescriptorSetArray final : public DeviceObject<DescriptorSetArray<G>>
{
	static constexpr size_t kDescriptorSetCount = 16;
	using ArrayType = std::array<DescriptorSetHandle<G>, kDescriptorSetCount>;

public:
	using SuperType = DeviceObject<DescriptorSetArray<G>>;
	using CreateDescType = ObjectTraits<DescriptorSetArray<G>>::CreateDescType;

	constexpr DescriptorSetArray() noexcept = default;
	DescriptorSetArray(DescriptorSetArray&& other) noexcept;
	DescriptorSetArray( // allocates array of descriptor set handles using single layout
		CreateDescType&& desc,
		const DescriptorSetLayout<G>& layout);
	~DescriptorSetArray();

	[[maybe_unused]] DescriptorSetArray& operator=(DescriptorSetArray&& other) noexcept;
	[[nodiscard]] const auto& operator[](uint8_t index) const { return myDescriptorSets[index]; };

	void Swap(DescriptorSetArray& rhs) noexcept;
	friend void Swap(DescriptorSetArray& lhs, DescriptorSetArray& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] static constexpr auto Capacity() { return kDescriptorSetCount; }

private:
	DescriptorSetArray( // takes ownership of provided descriptor set handles
		CreateDescType&& desc,
		ArrayType&& descriptorSetHandles);

	ArrayType myDescriptorSets;
};

template <GraphicsApi G>
using DescriptorSetArrayList = std::list<std::tuple<DescriptorSetArray<G>, uint8_t>>;

template <GraphicsApi G>
struct DescriptorUpdateTemplateCreateDesc final : DeviceObjectCreateDesc<G>
{
	DescriptorUpdateTemplateType<G> templateType{};
	DescriptorSetLayoutHandle<G> descriptorSetLayout{};
	PipelineBindPoint<G> pipelineBindPoint{};
	PipelineLayoutHandle<G> pipelineLayout{};
	uint32_t set = 0UL;
};

template <GraphicsApi G>
class DescriptorUpdateTemplate;

template <GraphicsApi G>
struct ObjectTraits<DescriptorUpdateTemplate<G>>
{
	using CreateDescType = DescriptorUpdateTemplateCreateDesc<G>;
};

template <GraphicsApi G>
class DescriptorUpdateTemplate final : public DeviceObject<DescriptorUpdateTemplate<G>>
{
public:
	using SuperType = DeviceObject<DescriptorUpdateTemplate<G>>;
	using CreateDescType = ObjectTraits<DescriptorUpdateTemplate<G>>::CreateDescType;

	constexpr DescriptorUpdateTemplate() noexcept = default;
	DescriptorUpdateTemplate(DescriptorUpdateTemplate&& other) noexcept;
	explicit DescriptorUpdateTemplate(CreateDescType&& desc);
	~DescriptorUpdateTemplate();

	[[maybe_unused]] DescriptorUpdateTemplate& operator=(DescriptorUpdateTemplate&& other) noexcept;
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

	[[nodiscard]] const auto& GetEntries() const noexcept { return myEntries; }

	void SetEntries(std::vector<DescriptorUpdateTemplateEntry<G>>&& entries);

private:
	void InternalDestroyTemplate();
	DescriptorUpdateTemplate( // takes ownership of provided handle
		CreateDescType&& desc,
		DescriptorUpdateTemplateHandle<G>&& handle);

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
	core::RangeSet<uint32_t>>; // array ranges

template <GraphicsApi G>
using BindingsMap = std::flat_map<uint32_t, BindingValue<G>>;

template <GraphicsApi G>
using BindingsData = std::vector<BindingVariant<G>>;

enum class DescriptorSetStatus : uint8_t
{
	kDirty,
	kReady
};

template <GraphicsApi G>
using DescriptorSetState = std::tuple<
	core::UpgradableSharedMutex,
	DescriptorSetStatus,
	BindingsMap<G>,
	BindingsData<G>,
	DescriptorUpdateTemplate<G>,
	std::optional<DescriptorSetArrayList<G>>>; // if std::nullopt -> uses push descriptors

} // namespace rhi

#include "descriptorset.inl"
