namespace rhi
{

inline IObject::~IObject() = default;

template <typename DerivedType>
void Object<DerivedType>::Swap(Object<DerivedType>& other) noexcept
{
	std::swap(myDesc, other.myDesc);
}

template <typename DerivedType>
Object<DerivedType>& Object<DerivedType>::operator=(Object<DerivedType>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <typename DerivedType>
Object<DerivedType>::Object(Object<DerivedType>&& other) noexcept
{
	Swap(other);
}

template <typename DerivedType>
Object<DerivedType>::Object(typename ObjectTraits<DerivedType>::CreateDescType&& desc)
	: myDesc(std::forward<typename ObjectTraits<DerivedType>::CreateDescType>(desc))
{
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	ENSUREF(IsValid(), "rhi object \"{}\" created without a uuid", myDesc.name);
	ENSUREF(gLiveObjectUuids.insert(myDesc.uuid).second, "rhi object \"{}\" created with the uuid of a live object", GetName());
#endif
}

template <typename DerivedType>
Object<DerivedType>::~Object()
{
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	if (IsValid())
		gLiveObjectUuids.erase(myDesc.uuid);
#endif
}

#define IMPLEMENT_OBJECT_GETINSTANCE(DerivedType) \
template <> \
Instance<ObjectTraits<DerivedType>::CreateDescType::GetApi()>& Object<DerivedType>::GetInstance() const noexcept \
{ \
	if (auto* rhi = GetRHI<ObjectTraits<DerivedType>::CreateDescType::GetApi()>()) \
		return rhi->GetInstance(); \
	static Instance<ObjectTraits<DerivedType>::CreateDescType::GetApi()> gNullInstance{}; \
	return gNullInstance; \
}

} // namespace rhi
