// the Metal backend's acceleration structures come with ray queries (docs/backends-plan.md, step 7): the device's limits
// say it has none (DeviceLimits::rayQuery), so gfx makes none; the empty ones it holds are moved and destroyed here
#include <rhi/metal/backend.h>

#include <rhi/accelerationstructure.h>
#include <rhi/commandencoder.h>
#include <rhi/rhi.h>

#include <core/assert.h>

#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(AccelerationStructure<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(AccelerationStructure<kMetal>);

namespace accelerationstructure
{

[[noreturn]] static void NotYet()
{
	ENSUREF(false, "the Metal backend's acceleration structures come with ray queries (docs/backends-plan.md, step 7)");
	__builtin_trap();
}

} // namespace accelerationstructure

template <>
AccelerationStructure<kMetal>::AccelerationStructure(CreateDescType&& desc)
	: DeviceObject<AccelerationStructure<kMetal>>(std::forward<CreateDescType>(desc))
{
	accelerationstructure::NotYet();
}

template <>
void AccelerationStructure<kMetal>::Swap(AccelerationStructure& rhs) noexcept
{
	DeviceObject<AccelerationStructure<kMetal>>::Swap(rhs);
	std::swap(myBuffer, rhs.myBuffer);
	std::swap(myHandle, rhs.myHandle);
	std::swap(myAddress, rhs.myAddress);
}

template <>
AccelerationStructure<kMetal>::AccelerationStructure(AccelerationStructure&& other) noexcept
{
	Swap(other);
}

template <>
AccelerationStructure<kMetal>::~AccelerationStructure()
{
	if (IsValid() && myHandle != nullptr)
		myHandle->release();
}

template <>
AccelerationStructure<kMetal>& AccelerationStructure<kMetal>::operator=(AccelerationStructure&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
AccelerationStructureSizes AccelerationStructure<kMetal>::GetSizes(
	const Device<kMetal>& /*device*/,
	AccelerationStructureType /*type*/,
	std::span<const AccelerationStructureGeometry> /*geometries*/,
	AccelerationStructureBuildFlags /*flags*/)
{
	accelerationstructure::NotYet();
}

template <>
void CommandEncoder<kMetal>::BuildAccelerationStructure(
	const AccelerationStructure<kMetal>& /*structure*/,
	std::span<const AccelerationStructureGeometry> /*geometries*/,
	AccelerationStructureBuildFlags /*flags*/,
	uint64_t /*scratchAddress*/,
	bool /*update*/) const
{
	accelerationstructure::NotYet();
}

template <>
void CommandEncoder<kMetal>::CopyAccelerationStructure(
	const AccelerationStructure<kMetal>& /*source*/, const AccelerationStructure<kMetal>& /*destination*/, bool /*compact*/) const
{
	accelerationstructure::NotYet();
}

} // namespace rhi
