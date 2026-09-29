#pragma once

#include <core/upgradablesharedmutex.h>
#include <core/utils.h>
#include <rhi/capi.h>
#include <rhi/instance.h>
#include <rhi/types.h>

#include <uuid.h>

#include <zpp_bits.h>

namespace uuids
{

// uuids::uuid keeps its bytes private, so zpp::bits can't reflect it via structured bindings;
// this ADL-found overload serializes it as its raw 16 bytes instead.
constexpr auto serialize(auto& archive, auto& self)//NOLINT(readability-identifier-naming)
	requires std::same_as<std::remove_cvref_t<decltype(self)>, uuid>
{
	std::array<uint8_t, sizeof(uuid)> bytes{};
	if constexpr (std::remove_cvref_t<decltype(archive)>::kind() == zpp::bits::kind::out)
	{
		std::ranges::transform(self.as_bytes(), bytes.begin(), [](std::byte byte) { return std::to_integer<uint8_t>(byte); });
		return archive(bytes);
	}
	else
	{
		auto result = archive(bytes);
		if (!zpp::bits::failure(result))
			self = uuid(bytes);
		return result;
	}
}

} // namespace uuids

namespace rhi
{

template <typename DerivedT>
struct ObjectTraits;

template <GraphicsApi G>
struct ObjectCreateDesc
{
	static consteval GraphicsApi GetApi() { return G; }
	InstanceHandle<G> instance{};
	uuids::uuid uuid{};

	// "instance" (and, in validation builds, "objectType"/"objectPointers") are runtime-only
	// handles/pointers that are not meaningful across a save/load round-trip, so only "name" is serialized.
	constexpr static auto serialize(auto& archive, auto& self)//NOLINT(readability-identifier-naming)
	{
		return archive(self.uuid);
	}
};

struct IObject
{
	virtual ~IObject() = 0;
	[[nodiscard]] virtual bool IsValid() const noexcept = 0;
};

template <typename DerivedType>
class Object : public IObject
{
public:
	using SuperType = IObject;
	using CreateDescType = ObjectTraits<DerivedType>::CreateDescType;
	
	[[nodiscard]] static consteval GraphicsApi GetApi() { return ObjectTraits<DerivedType>::CreateDescType::GetApi(); }

	Object(const Object&) = delete;
	~Object() override;
	[[nodiscard]] Object& operator=(const Object&) = delete;

	[[nodiscard]] bool IsValid() const noexcept override { return !!GetDesc().instance; }

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] auto GetName() const { return uuids::to_string(myDesc.uuid); }
	[[nodiscard]] Instance<GetApi()>& GetInstance() const noexcept;

	void Swap(Object& other) noexcept;

protected:
	constexpr Object() noexcept = default;
	explicit Object(ObjectTraits<DerivedType>::CreateDescType&& desc);
	Object(Object&& other) noexcept;

	[[maybe_unused]] Object& operator=(Object&& other) noexcept;

	[[nodiscard]] auto& InternalGetDesc() noexcept { return myDesc; }

private:
	ObjectTraits<DerivedType>::CreateDescType myDesc{};
};

} // namespace rhi

#include "object.inl"
