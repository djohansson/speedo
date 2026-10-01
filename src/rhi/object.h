#pragma once

#include <core/upgradablesharedmutex.h>
#include <core/utils.h>
#include <core/uuids_extra.h>
#include <rhi/capi.h>
#include <rhi/instance.h>
#include <rhi/types.h>

namespace rhi
{

template <typename DerivedT>
struct ObjectTraits;

template <GraphicsApi G>
struct ObjectCreateDesc
{
	uuids::uuid uuid{};
	InstanceHandle<G> instance{};

	static consteval GraphicsApi GetApi() { return G; }

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
	[[nodiscard]] virtual const uuids::uuid& GetUuid() const noexcept = 0;
	[[nodiscard]] virtual bool IsValid() const noexcept = 0;
};

template <typename DerivedType>
class Object : public IObject
{
public:
	// IObject
	~Object() override;
	[[nodiscard]] const uuids::uuid& GetUuid() const noexcept final { return myDesc.uuid; }
	[[nodiscard]] bool IsValid() const noexcept override { return GetUuid() != uuids::uuid{}; }

	// Object
	using SuperType = IObject;
	using CreateDescType = ObjectTraits<DerivedType>::CreateDescType;
	
	Object(const Object&) = delete;
	[[nodiscard]] Object& operator=(const Object&) = delete;

	[[nodiscard]] static consteval GraphicsApi GetApi() { return ObjectTraits<DerivedType>::CreateDescType::GetApi(); }
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
