// the D3D12 backend's acceleration structures: none until its ray queries (docs/backends-plan.md, step 6), which the
// device doesn't report until then (DeviceLimits::rayQuery), so that gfx makes none. their members, which gfx
// references, fail hard
#include <rhi/d3d12/backend.h>

#include <rhi/accelerationstructure.h>
#include <rhi/commandencoder.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>

#include <cstdlib>
#include <span>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(AccelerationStructure<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(AccelerationStructure<kD3D12>);

namespace accelerationstructure
{

[[noreturn]] static void NotYet()
{
	ENSUREF(false, "the D3D12 backend has no acceleration structures yet (docs/backends-plan.md, step 6)");
	std::abort();
}

} // namespace accelerationstructure

template <>
AccelerationStructure<kD3D12>::AccelerationStructure(CreateDescType&& desc)
	: DeviceObject<AccelerationStructure<kD3D12>>(std::forward<CreateDescType>(desc))
{
	accelerationstructure::NotYet();
}

template <>
void AccelerationStructure<kD3D12>::Swap(AccelerationStructure& rhs) noexcept
{
	DeviceObject<AccelerationStructure<kD3D12>>::Swap(rhs);
	std::swap(myBuffer, rhs.myBuffer);
	std::swap(myHandle, rhs.myHandle);
	std::swap(myAddress, rhs.myAddress);
}

template <>
AccelerationStructure<kD3D12>::AccelerationStructure(AccelerationStructure&& other) noexcept
{
	Swap(other);
}

template <>
AccelerationStructure<kD3D12>::~AccelerationStructure() = default;

template <>
AccelerationStructure<kD3D12>& AccelerationStructure<kD3D12>::operator=(AccelerationStructure&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
AccelerationStructureSizes AccelerationStructure<kD3D12>::GetSizes(
	const Device<kD3D12>& /*device*/,
	AccelerationStructureType /*type*/,
	std::span<const AccelerationStructureGeometry> /*geometries*/,
	AccelerationStructureBuildFlags /*flags*/)
{
	accelerationstructure::NotYet();
}

template <>
void CommandEncoder<kD3D12>::BuildAccelerationStructure(
	const AccelerationStructure<kD3D12>& /*structure*/,
	std::span<const AccelerationStructureGeometry> /*geometries*/,
	AccelerationStructureBuildFlags /*flags*/,
	uint64_t /*scratchAddress*/,
	bool /*update*/) const
{
	accelerationstructure::NotYet();
}

template <>
void CommandEncoder<kD3D12>::CopyAccelerationStructure(
	const AccelerationStructure<kD3D12>& /*source*/, const AccelerationStructure<kD3D12>& /*destination*/, bool /*compact*/) const
{
	accelerationstructure::NotYet();
}

} // namespace rhi
