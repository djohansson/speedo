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
{}

template <typename DerivedType>
Object<DerivedType>::~Object()
{}

#define IMPLEMENT_OBJECT_GETINSTANCE(DerivedType) \
template <> \
Instance<ObjectTraits<DerivedType>::CreateDescType::GetApi()>& Object<DerivedType>::GetInstance() const noexcept \
{ \
	if (auto app = std::static_pointer_cast<RHIApplication>(core::Application::Get())) \
		return app->GetRHI<ObjectTraits<DerivedType>::CreateDescType::GetApi()>().GetInstance(); \
	static Instance<ObjectTraits<DerivedType>::CreateDescType::GetApi()> gNullInstance{}; \
	return gNullInstance; \
}

} // namespace rhi
