#include <rhi/accelerationstructure.h>
#include <rhi/commandencoder.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/querypool.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

#include <core/assert.h>

#include <format>
#include <utility>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(AccelerationStructure<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(AccelerationStructure<kVk>);

namespace accelerationstructure
{

// a build's geometries in vulkan's terms, and each one's primitive count
struct Geometries
{
	std::vector<VkAccelerationStructureGeometryKHR> geometries;
	std::vector<uint32_t> counts;
};

static Geometries ToVk(std::span<const AccelerationStructureGeometry> geometries)
{
	Geometries result;
	for (const auto& geometry : geometries)
	{
		VkAccelerationStructureGeometryKHR& vkGeometry =
			result.geometries.emplace_back(VkAccelerationStructureGeometryKHR{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR});
		if (const auto* triangles = std::get_if<AccelerationStructureTriangles>(&geometry))
		{
			vkGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
			vkGeometry.geometry.triangles = VkAccelerationStructureGeometryTrianglesDataKHR{
				.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
				.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
				.vertexData = {.deviceAddress = triangles->vertexAddress},
				.vertexStride = triangles->vertexStride,
				.maxVertex = triangles->triangleCount > 0 ? (triangles->triangleCount * 3) - 1 : 0,
				.indexType = VK_INDEX_TYPE_NONE_KHR};
			vkGeometry.flags = triangles->opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
			result.counts.push_back(triangles->triangleCount);
		}
		else
		{
			const auto& instances = std::get<AccelerationStructureInstances>(geometry);
			vkGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
			vkGeometry.geometry.instances = VkAccelerationStructureGeometryInstancesDataKHR{
				.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
				.arrayOfPointers = VK_FALSE,
				.data = {.deviceAddress = instances.address}};
			result.counts.push_back(instances.count);
		}
	}
	return result;
}

static VkBuildAccelerationStructureFlagsKHR ToVk(AccelerationStructureBuildFlags flags)
{
	VkBuildAccelerationStructureFlagsKHR result = 0;
	if (Any(flags & AccelerationStructureBuildFlags::kPreferFastTrace))
		result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	if (Any(flags & AccelerationStructureBuildFlags::kPreferFastBuild))
		result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	if (Any(flags & AccelerationStructureBuildFlags::kAllowUpdate))
		result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	if (Any(flags & AccelerationStructureBuildFlags::kAllowCompaction))
		result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
	return result;
}

static VkAccelerationStructureTypeKHR ToVk(AccelerationStructureType type)
{
	return type == AccelerationStructureType::kTopLevel ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
														: VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
}

} // namespace accelerationstructure

template <>
AccelerationStructure<kVk>::AccelerationStructure(CreateDescType&& desc)
	: DeviceObject(std::forward<CreateDescType>(desc))
{
	ENSUREF(gVkCreateAccelerationStructureKHR != nullptr, "acceleration structures without ray queries (DeviceLimits::rayQuery)");
	myBuffer = Buffer<kVk>(BufferCreateDesc<kVk>{
		CreateDeviceObjectCreateDesc(std::format("{} storage", GetName())),
		std::max<uint64_t>(GetDesc().size, 1),
		BufferUsage::kAccelerationStructureStorage | BufferUsage::kDeviceAddress,
		MemoryProperty::kDeviceLocal});
	VkAccelerationStructureCreateInfoKHR createInfo{
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
		.buffer = myBuffer,
		.offset = 0,
		.size = GetDesc().size,
		.type = accelerationstructure::ToVk(GetDesc().type)};
	VK_CHECK(gVkCreateAccelerationStructureKHR(GetDevice(), &createInfo, &GetInstance().GetHostAllocationCallbacks(), &myHandle));
	Track(GetDevice(), VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, myHandle, GetDebugName(GetDesc()));
	VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = myHandle};
	myAddress = gVkGetAccelerationStructureDeviceAddressKHR(GetDevice(), &addressInfo);
}

template <>
void AccelerationStructure<kVk>::Swap(AccelerationStructure& rhs) noexcept
{
	DeviceObject<AccelerationStructure<kVk>>::Swap(rhs);
	std::swap(myBuffer, rhs.myBuffer);
	std::swap(myHandle, rhs.myHandle);
	std::swap(myAddress, rhs.myAddress);
}

template <>
AccelerationStructure<kVk>::AccelerationStructure(AccelerationStructure<kVk>&& other) noexcept
{
	Swap(other);
}

template <>
AccelerationStructure<kVk>::~AccelerationStructure()
{
	if (!IsValid())
		return;
	Untrack(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, myHandle);
	gVkDestroyAccelerationStructureKHR(GetDevice(), myHandle, &GetInstance().GetHostAllocationCallbacks());
}

template <>
AccelerationStructure<kVk>& AccelerationStructure<kVk>::operator=(AccelerationStructure<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
AccelerationStructureSizes AccelerationStructure<kVk>::GetSizes(
	const Device<kVk>& device,
	AccelerationStructureType type,
	std::span<const AccelerationStructureGeometry> geometries,
	AccelerationStructureBuildFlags flags)
{
	ENSUREF(gVkGetAccelerationStructureBuildSizesKHR != nullptr, "acceleration structures without ray queries (DeviceLimits::rayQuery)");
	auto [vkGeometries, counts] = accelerationstructure::ToVk(geometries);
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
		.type = accelerationstructure::ToVk(type),
		.flags = accelerationstructure::ToVk(flags),
		.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
		.geometryCount = static_cast<uint32_t>(vkGeometries.size()),
		.pGeometries = vkGeometries.data()};
	VkAccelerationStructureBuildSizesInfoKHR sizes{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
	gVkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, counts.data(), &sizes);
	return {.structureSize = sizes.accelerationStructureSize, .buildScratchSize = sizes.buildScratchSize, .updateScratchSize = sizes.updateScratchSize};
}

template <>
void CommandEncoder<kVk>::BuildAccelerationStructure(
	const AccelerationStructure<kVk>& structure,
	std::span<const AccelerationStructureGeometry> geometries,
	AccelerationStructureBuildFlags flags,
	uint64_t scratchAddress,
	bool update) const
{
	auto [vkGeometries, counts] = accelerationstructure::ToVk(geometries);
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
		.type = accelerationstructure::ToVk(structure.GetDesc().type),
		.flags = accelerationstructure::ToVk(flags),
		.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
		.srcAccelerationStructure = update ? static_cast<VkAccelerationStructureKHR>(structure) : VK_NULL_HANDLE,
		.dstAccelerationStructure = structure,
		.geometryCount = static_cast<uint32_t>(vkGeometries.size()),
		.pGeometries = vkGeometries.data(),
		.scratchData = {.deviceAddress = scratchAddress}};
	std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
	ranges.reserve(counts.size());
	for (uint32_t count : counts)
		ranges.push_back({.primitiveCount = count, .primitiveOffset = 0, .firstVertex = 0, .transformOffset = 0});
	const VkAccelerationStructureBuildRangeInfoKHR* rangeInfos = ranges.data();
	gVkCmdBuildAccelerationStructuresKHR(myCmd, 1, &buildInfo, &rangeInfos);
}

template <>
void CommandEncoder<kVk>::WriteAccelerationStructureCompactedSize(
	const AccelerationStructure<kVk>& structure, const QueryPool<kVk>& pool, uint32_t query) const
{
	VkAccelerationStructureKHR handle = structure;
	gVkCmdWriteAccelerationStructuresPropertiesKHR(myCmd, 1, &handle, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, pool, query);
}

template <>
void CommandEncoder<kVk>::CopyAccelerationStructure(
	const AccelerationStructure<kVk>& source, const AccelerationStructure<kVk>& destination, bool compact) const
{
	VkCopyAccelerationStructureInfoKHR info{
		.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
		.src = source,
		.dst = destination,
		.mode = compact ? VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR : VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR};
	gVkCmdCopyAccelerationStructureKHR(myCmd, &info);
}

} // namespace rhi
