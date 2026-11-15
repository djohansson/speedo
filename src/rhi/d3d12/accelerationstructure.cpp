// the D3D12 backend's acceleration structures (DXR 1.1's, the shaders' inline ray queries: docs/backends-plan.md, step
// 6): a structure is a device local buffer of its own (D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE, enhanced
// barriers' buffers having no layout) and its gpu address, which a top level instance names it by and a set binds it as
// (a shader resource view of the address). the instances are Vulkan's layout already (D3D12_RAYTRACING_INSTANCE_DESC's:
// rhi's AccelerationStructureInstance), read where gfx puts them; builds, updates (a build of the structure from
// itself) and copies (compacting ones too) are the list's
#include <rhi/d3d12/backend.h>

#include <rhi/accelerationstructure.h>
#include <rhi/commandencoder.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <format>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(AccelerationStructure<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(AccelerationStructure<kD3D12>);

namespace accelerationstructure
{

// rhi's instances are D3D12's (and Vulkan's) as the device reads them
static_assert(sizeof(AccelerationStructureInstance) == sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

static D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS ToD3D12(AccelerationStructureBuildFlags flags)
{
	auto result = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	if (Any(flags & AccelerationStructureBuildFlags::kPreferFastTrace))
		result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	if (Any(flags & AccelerationStructureBuildFlags::kPreferFastBuild))
		result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
	if (Any(flags & AccelerationStructureBuildFlags::kAllowUpdate))
		result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
	if (Any(flags & AccelerationStructureBuildFlags::kAllowCompaction))
		result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
	return result;
}

// a build's inputs: a bottom level structure's triangles (geometryDescs, which inputs points into), or a top level one's
// instances
struct Inputs
{
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
	std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
};

static Inputs InputsOf(AccelerationStructureType type, std::span<const AccelerationStructureGeometry> geometries, AccelerationStructureBuildFlags flags)
{
	Inputs result;
	result.inputs.Flags = ToD3D12(flags);
	result.inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	if (type == AccelerationStructureType::kTopLevel)
	{
		ENSUREF(geometries.size() == 1, "a top level structure of {} sets of instances (one)", geometries.size());
		const auto* instances = std::get_if<AccelerationStructureInstances>(geometries.data());
		ENSUREF(instances != nullptr, "a top level structure of triangles");
		ENSUREF(instances->address % D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT == 0, "a top level structure's instances at {:#x} (16 bytes aligned)", instances->address);
		result.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		result.inputs.NumDescs = instances->count;
		result.inputs.InstanceDescs = instances->address;
		return result;
	}
	result.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	result.geometryDescs.reserve(geometries.size());
	for (const auto& geometry : geometries)
	{
		const auto* triangles = std::get_if<AccelerationStructureTriangles>(&geometry);
		ENSUREF(triangles != nullptr, "a bottom level structure of instances");
		ENSUREF(
			triangles->transformAddress % D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT == 0,
			"a bottom level structure's transform at {:#x} (16 bytes aligned)",
			triangles->transformAddress);
		const bool indexed = triangles->indexAddress != 0;
		result.geometryDescs.push_back(D3D12_RAYTRACING_GEOMETRY_DESC{
			.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
			.Flags = triangles->opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
			.Triangles = {
				.Transform3x4 = triangles->transformAddress,
				.IndexFormat = indexed ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN,
				.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
				.IndexCount = indexed ? triangles->triangleCount * 3 : 0,
				.VertexCount = triangles->vertexCount,
				.IndexBuffer = triangles->indexAddress,
				.VertexBuffer = {.StartAddress = triangles->vertexAddress, .StrideInBytes = triangles->vertexStride},
			},
		});
	}
	result.inputs.NumDescs = static_cast<UINT>(result.geometryDescs.size());
	result.inputs.pGeometryDescs = result.geometryDescs.data();
	return result;
}

} // namespace accelerationstructure

template <>
AccelerationStructure<kD3D12>::AccelerationStructure(CreateDescType&& desc)
	: DeviceObject<AccelerationStructure<kD3D12>>(std::forward<CreateDescType>(desc))
{
	ENSUREF(GetDevice().GetLimits().rayQuery, "acceleration structures without ray queries (DeviceLimits::rayQuery)");
	myBuffer = Buffer<kD3D12>(BufferCreateDesc<kD3D12>{
		CreateDeviceObjectCreateDesc(std::format("{} storage", GetName())),
		std::max<uint64_t>(GetDesc().size, 1),
		BufferUsage::kAccelerationStructureStorage | BufferUsage::kDeviceAddress,
		MemoryProperty::kDeviceLocal});
	myAddress = myBuffer.GetDeviceAddress();
	myHandle = new d3d12::AccelerationStructureObject{.buffer = myBuffer, .address = myAddress}; //NOLINT(cppcoreguidelines-owning-memory)
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
AccelerationStructure<kD3D12>::~AccelerationStructure()
{
	delete myHandle; //NOLINT(cppcoreguidelines-owning-memory): the buffer is myBuffer's
}

template <>
AccelerationStructure<kD3D12>& AccelerationStructure<kD3D12>::operator=(AccelerationStructure&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
AccelerationStructureSizes AccelerationStructure<kD3D12>::GetSizes(
	const Device<kD3D12>& device,
	AccelerationStructureType type,
	std::span<const AccelerationStructureGeometry> geometries,
	AccelerationStructureBuildFlags flags)
{
	const auto inputs = accelerationstructure::InputsOf(type, geometries, flags);
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
	static_cast<ID3D12Device10*>(device)->GetRaytracingAccelerationStructurePrebuildInfo(&inputs.inputs, &info);
	return {
		.structureSize = info.ResultDataMaxSizeInBytes,
		.buildScratchSize = info.ScratchDataSizeInBytes,
		.updateScratchSize = info.UpdateScratchDataSizeInBytes,
	};
}

template <>
void CommandEncoder<kD3D12>::BuildAccelerationStructure(
	const AccelerationStructure<kD3D12>& structure,
	std::span<const AccelerationStructureGeometry> geometries,
	AccelerationStructureBuildFlags flags,
	uint64_t scratchAddress,
	bool update) const
{
	ZoneScopedN("CommandEncoder::BuildAccelerationStructure");

	ENSUREF(
		scratchAddress % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT == 0,
		"an acceleration structure build's scratch at {:#x} (256 bytes aligned)",
		scratchAddress);
	auto inputs = accelerationstructure::InputsOf(structure.GetDesc().type, geometries, flags);
	if (update)
		inputs.inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{
		.DestAccelerationStructureData = structure.GetDeviceAddress(),
		.Inputs = inputs.inputs,
		.SourceAccelerationStructureData = update ? structure.GetDeviceAddress() : 0,
		.ScratchAccelerationStructureData = scratchAddress,
	};
	myCmd->list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
}

template <>
void CommandEncoder<kD3D12>::CopyAccelerationStructure(
	const AccelerationStructure<kD3D12>& source, const AccelerationStructure<kD3D12>& destination, bool compact) const
{
	myCmd->list->CopyRaytracingAccelerationStructure(
		destination.GetDeviceAddress(),
		source.GetDeviceAddress(),
		compact ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
}

} // namespace rhi
