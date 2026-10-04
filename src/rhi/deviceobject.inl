namespace rhi
{

template <typename DerivedType>
DeviceObject<DerivedType>::DeviceObject(DeviceObject<DerivedType>&& other) noexcept
{
	Swap(other);
}

template <typename DerivedType>
DeviceObject<DerivedType>::DeviceObject(CreateDescType&& desc)
	: SuperType(std::forward<CreateDescType>(desc))
{}

template <typename DerivedType>
DeviceObject<DerivedType>::~DeviceObject() = default;

template <typename DerivedType>
DeviceObject<DerivedType>& DeviceObject<DerivedType>::operator=(DeviceObject<DerivedType>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <typename DerivedType>
void DeviceObject<DerivedType>::Swap(DeviceObject<DerivedType>& other) noexcept
{
	SuperType::Swap(other);
}

#define IMPLEMENT_DEVICEOBJECT_GETDEVICE(DerivedType) \
template <> \
Device<DeviceObject<DerivedType>::GetApi()>& DeviceObject<DerivedType>::GetDevice(DeviceHandle<GetApi()> deviceHandle) const noexcept \
{ \
	if (auto* rhi = GetRHI<GetApi()>()) \
		return rhi->GetDevice(deviceHandle ? deviceHandle : SuperType::GetDesc().device); \
	static Device<GetApi()> gNullDevice{}; \
	return gNullDevice; \
}

} // namespace rhi
