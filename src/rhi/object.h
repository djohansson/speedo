#pragma once

#include <core/assert.h>
#include <core/uuids_extra.h>
#include <rhi/capi.h>
#include <rhi/instance.h>
#include <rhi/types.h>

#include <string>

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
#include <mutex>
#include <parallel_hashmap/phmap.h>
#endif

namespace rhi
{

template <typename DerivedT>
struct ObjectTraits;

template <GraphicsApi G>
struct ObjectCreateDesc
{
	// identifies the object: must be unique among live objects, and not nil (see Object::IsValid). create descs through
	// the Create*ObjectCreateDesc helpers, which assign a new random one; validation builds check both on construction.
	uuids::uuid uuid{};
	InstanceHandle<G> instance{};
	std::string name; // for debugging, e.g. the vulkan object names. not unique

	static consteval GraphicsApi GetApi() { return G; }

	// "instance" and "name" are runtime-only, so only "uuid" is serialized. a desc read back from a file shares the
	// uuid of the object it was saved from, so loaders replace it with a new one before constructing an object.
	constexpr static auto serialize(auto& archive, auto& self)//NOLINT(readability-identifier-naming)
	{
		return archive(self.uuid);
	}
};

// the desc's name, or its uuid if it has none: never empty for a valid desc
template <GraphicsApi G>
[[nodiscard]] std::string GetDebugName(const ObjectCreateDesc<G>& desc)
{
	return desc.name.empty() ? uuids::to_string(desc.uuid) : desc.name;
}

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
// uuids of the live objects, to check they are unique. sharded, with a lock per shard.
inline phmap::parallel_flat_hash_set<
	uuids::uuid,
	std::hash<uuids::uuid>,
	std::equal_to<>,
	std::allocator<uuids::uuid>,
	4,
	std::mutex> gLiveObjectUuids;
#endif

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
	[[nodiscard]] bool IsValid() const noexcept final { return !GetUuid().is_nil(); }

	// Object
	using SuperType = IObject;
	using CreateDescType = ObjectTraits<DerivedType>::CreateDescType;
	
	Object(const Object&) = delete;
	[[nodiscard]] Object& operator=(const Object&) = delete;

	[[nodiscard]] static consteval GraphicsApi GetApi() { return ObjectTraits<DerivedType>::CreateDescType::GetApi(); }
	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] std::string GetName() const { return GetDebugName(myDesc); }
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
