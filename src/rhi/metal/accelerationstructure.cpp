// the Metal backend's acceleration structures (docs/backends-plan.md, step 7): MTL::AccelerationStructure objects, built,
// refit, compacted and copied on the command buffer's acceleration structure encoder. rhi names a build's inputs by gpu
// address, which Metal takes as a buffer and an offset (metal::FindBuffer). a bottom level structure's triangles are
// Vulkan's (float3 positions, 32 bit indices or none, a 3x4 row major transform); a top level structure's instances are
// rhi's (Vulkan's 64 bytes, AccelerationStructureInstance), which a kernel converts into Metal's indirect instance
// descriptors in the build's scratch memory before the build (they may be written on the gpu: compaction rewrites their
// structure's id with a copy). a structure's address (GetDeviceAddress) is its resource id, what Metal's indirect
// instances name it by
#include <rhi/metal/backend.h>

#include <rhi/accelerationstructure.h>
#include <rhi/commandencoder.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(AccelerationStructure<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(AccelerationStructure<kMetal>);

namespace metal
{

namespace
{

std::mutex gBottomLevelsMutex;
std::vector<MTL::Resource*> gBottomLevels; // the live bottom level structures (not retained)

} // namespace

void UseBottomLevels(MTL::ComputeCommandEncoder* encoder)
{
	std::scoped_lock lock{gBottomLevelsMutex};
	if (!gBottomLevels.empty())
		encoder->useResources(gBottomLevels.data(), gBottomLevels.size(), MTL::ResourceUsageRead);
}

void UseBottomLevels(MTL::RenderCommandEncoder* encoder)
{
	std::scoped_lock lock{gBottomLevelsMutex};
	if (!gBottomLevels.empty())
		encoder->useResources(
			gBottomLevels.data(), gBottomLevels.size(), MTL::ResourceUsageRead, MTL::RenderStageVertex | MTL::RenderStageFragment);
}

void UseBottomLevels(MTL::AccelerationStructureCommandEncoder* encoder)
{
	std::scoped_lock lock{gBottomLevelsMutex};
	if (!gBottomLevels.empty())
		encoder->useResources(gBottomLevels.data(), gBottomLevels.size(), MTL::ResourceUsageRead);
}

bool RayQueriesEnabled()
{
	static const bool kEnabled = []
	{
		const char* rayQuery = std::getenv("SPEEDO_RAY_QUERY");
		return rayQuery != nullptr && std::string_view(rayQuery) == "1";
	}();
	return kEnabled;
}

} // namespace metal

namespace accelerationstructure
{

constexpr uint64_t kScratchAlignment = 256; // Metal's scratch offsets' (Device::GetLimits' accelerationStructureScratchAlignment)

[[nodiscard]] static uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

// a top level build's scratch memory: the instances converted (Metal's indirect instance descriptors), their count (a
// uint), then Metal's own scratch
struct TopLevelScratch
{
	uint64_t instances = 0;
	uint64_t count = 0;
	uint64_t metal = 0;
};

[[nodiscard]] static TopLevelScratch TopLevelLayout(uint32_t instanceCount)
{
	TopLevelScratch layout;
	layout.count = AlignUp(uint64_t{instanceCount} * sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor), 16);
	layout.metal = AlignUp(layout.count + sizeof(uint32_t), kScratchAlignment);
	return layout;
}

[[nodiscard]] static MTL::AccelerationStructureUsage Usage(AccelerationStructureBuildFlags flags) noexcept
{
	MTL::AccelerationStructureUsage usage = MTL::AccelerationStructureUsageNone;
	if (Any(flags & AccelerationStructureBuildFlags::kPreferFastBuild))
		usage |= MTL::AccelerationStructureUsagePreferFastBuild;
	if (Any(flags & AccelerationStructureBuildFlags::kAllowUpdate))
		usage |= MTL::AccelerationStructureUsageRefit;
	return usage;
}

// a bottom level structure's descriptor: its triangles, each geometry's buffers found by address
[[nodiscard]] static MTL::AccelerationStructureDescriptor* NewBottomLevel(
	std::span<const AccelerationStructureGeometry> geometries, AccelerationStructureBuildFlags flags)
{
	std::vector<NS::Object*> triangles;
	triangles.reserve(geometries.size());
	for (const auto& geometry : geometries)
	{
		const auto* input = std::get_if<AccelerationStructureTriangles>(&geometry);
		ENSUREF(input != nullptr, "a bottom level structure of instances");
		auto* descriptor = MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init();
		const auto [vertices, verticesOffset] = metal::FindBuffer(input->vertexAddress);
		descriptor->setVertexBuffer(vertices);
		descriptor->setVertexBufferOffset(verticesOffset);
		descriptor->setVertexStride(input->vertexStride);
		descriptor->setVertexFormat(MTL::AttributeFormatFloat3);
		if (input->indexAddress != 0)
		{
			const auto [indices, indicesOffset] = metal::FindBuffer(input->indexAddress);
			descriptor->setIndexBuffer(indices);
			descriptor->setIndexBufferOffset(indicesOffset);
			descriptor->setIndexType(MTL::IndexTypeUInt32);
		}
		if (input->transformAddress != 0)
		{
			const auto [transforms, transformsOffset] = metal::FindBuffer(input->transformAddress);
			descriptor->setTransformationMatrixBuffer(transforms);
			descriptor->setTransformationMatrixBufferOffset(transformsOffset);
			descriptor->setTransformationMatrixLayout(MTL::MatrixLayoutRowMajor); // Vulkan's 3x4 rows
		}
		descriptor->setTriangleCount(input->triangleCount);
		descriptor->setOpaque(input->opaque);
		triangles.push_back(descriptor);
	}
	auto* descriptor = MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init();
	descriptor->setGeometryDescriptors(NS::Array::array(triangles.data(), triangles.size()));
	descriptor->setUsage(Usage(flags));
	for (auto* triangle : triangles)
		triangle->release();
	return descriptor;
}

// a top level structure's descriptor: count instances at most, Metal's indirect ones (and their count) in scratch at
// the layout's offsets (no buffer for a size's query)
[[nodiscard]] static MTL::AccelerationStructureDescriptor* NewTopLevel(
	uint32_t count, AccelerationStructureBuildFlags flags, MTL::Buffer* scratch = nullptr, uint64_t scratchOffset = 0)
{
	auto* descriptor = MTL::IndirectInstanceAccelerationStructureDescriptor::alloc()->init();
	descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeIndirect);
	descriptor->setInstanceDescriptorStride(sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor));
	descriptor->setMaxInstanceCount(count);
	descriptor->setUsage(Usage(flags));
	if (scratch != nullptr)
	{
		const auto layout = TopLevelLayout(count);
		descriptor->setInstanceDescriptorBuffer(scratch);
		descriptor->setInstanceDescriptorBufferOffset(scratchOffset + layout.instances);
		descriptor->setInstanceCountBuffer(scratch);
		descriptor->setInstanceCountBufferOffset(scratchOffset + layout.count);
	}
	return descriptor;
}

// rhi's instances (Vulkan's) as Metal's indirect instance descriptors: the transform's rows as Metal's columns, the
// custom index as the user id (CommittedInstanceID), the mask, and the instance flags as Metal's options (the same bits:
// culling disabled, front facing flipped, opaque, not opaque). an instance without a structure (address 0) is masked out
// with another's structure in its place (the instances keep their indices, CommittedInstanceIndex); with none, none count
constexpr const char* kInstanceSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VulkanInstance
{
	float transform[12];
	uint customIndexAndMask;
	uint shaderBindingTableOffsetAndFlags;
	ulong structureAddress;
};

struct MetalInstance
{
	packed_float3 columns[4];
	uint options;
	uint mask;
	uint intersectionFunctionTableOffset;
	uint userID;
	ulong accelerationStructureID;
};

kernel void ConvertInstances(
	device const VulkanInstance* source [[buffer(0)]],
	device MetalInstance* destination [[buffer(1)]],
	device uint* destinationCount [[buffer(2)]],
	constant uint& count [[buffer(3)]])
{
	ulong any = 0;
	for (uint instanceIt = 0; instanceIt < count && any == 0; instanceIt++)
		any = source[instanceIt].structureAddress;
	for (uint instanceIt = 0; instanceIt < count; instanceIt++)
	{
		VulkanInstance instance = source[instanceIt];
		MetalInstance converted;
		for (uint column = 0; column < 4; column++)
			converted.columns[column] = packed_float3(instance.transform[column], instance.transform[4 + column], instance.transform[8 + column]);
		converted.options = instance.shaderBindingTableOffsetAndFlags >> 24;
		converted.mask = instance.structureAddress != 0 ? instance.customIndexAndMask >> 24 : 0;
		converted.intersectionFunctionTableOffset = 0;
		converted.userID = instance.customIndexAndMask & 0xFFFFFF;
		converted.accelerationStructureID = instance.structureAddress != 0 ? instance.structureAddress : any;
		destination[instanceIt] = converted;
	}
	*destinationCount = any != 0 ? count : 0;
}
)";
static_assert(sizeof(AccelerationStructureInstance) == 64 && sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor) == 72);

// the conversion's pipeline, the device's, made at its first top level build
[[nodiscard]] static MTL::ComputePipelineState* InstancePipeline(metal::AllocatorObject& allocator)
{
	std::scoped_lock lock{allocator.mutex};
	if (allocator.instancePipeline != nullptr)
		return allocator.instancePipeline;

	ZoneScopedN("AccelerationStructure::InstancePipeline");

	auto* source = NS::String::alloc()->init(kInstanceSource, NS::UTF8StringEncoding);
	auto* options = MTL::CompileOptions::alloc()->init();
	options->setLanguageVersion(MTL::LanguageVersion3_2);
	NS::Error* error = nullptr;
	MTL::Library* library = allocator.device->newLibrary(source, options, &error);
	options->release();
	source->release();
	ENSUREF(
		library != nullptr,
		"Failed to compile the acceleration structure instances' conversion: {}",
		error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");
	MTL::Function* function = library->newFunction(NS::String::string("ConvertInstances", NS::UTF8StringEncoding));
	allocator.instancePipeline = allocator.device->newComputePipelineState(function, &error);
	function->release();
	library->release();
	ENSUREF(
		allocator.instancePipeline != nullptr,
		"Failed to make the acceleration structure instances' conversion pipeline: {}",
		error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");
	return allocator.instancePipeline;
}

} // namespace accelerationstructure

template <>
AccelerationStructure<kMetal>::AccelerationStructure(CreateDescType&& desc)
	: DeviceObject<AccelerationStructure<kMetal>>(std::forward<CreateDescType>(desc))
{
	const auto& structureDesc = GetDesc();
	ENSUREF(structureDesc.size > 0, "an acceleration structure of no size");
	myHandle = structureDesc.device->newAccelerationStructure(structureDesc.size);
	ENSUREF(myHandle != nullptr, "Failed to make a Metal acceleration structure of {} bytes", structureDesc.size);
	myHandle->setLabel(NS::String::string(structureDesc.name.c_str(), NS::UTF8StringEncoding));
	myAddress = myHandle->gpuResourceID()._impl;
	if (structureDesc.type == AccelerationStructureType::kBottomLevel)
	{
		std::scoped_lock lock{metal::gBottomLevelsMutex};
		metal::gBottomLevels.push_back(myHandle);
	}
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
	if (!IsValid() || myHandle == nullptr)
		return;
	{
		std::scoped_lock lock{metal::gBottomLevelsMutex};
		if (auto found = std::ranges::find(metal::gBottomLevels, myHandle); found != metal::gBottomLevels.end())
			metal::gBottomLevels.erase(found);
	}
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
	const Device<kMetal>& device,
	AccelerationStructureType type,
	std::span<const AccelerationStructureGeometry> geometries,
	AccelerationStructureBuildFlags flags)
{
	using namespace accelerationstructure;

	MTL::AccelerationStructureDescriptor* descriptor = nullptr;
	uint64_t scratchBefore = 0;
	if (type == AccelerationStructureType::kTopLevel)
	{
		ENSUREF(geometries.size() == 1, "a top level structure of {} sets of instances (one)", geometries.size());
		const auto* instances = std::get_if<AccelerationStructureInstances>(geometries.data());
		ENSUREF(instances != nullptr, "a top level structure of triangles");
		descriptor = NewTopLevel(instances->count, flags);
		scratchBefore = TopLevelLayout(instances->count).metal;
	}
	else
		descriptor = NewBottomLevel(geometries, flags);
	const MTL::AccelerationStructureSizes sizes = static_cast<MTL::Device*>(device)->accelerationStructureSizes(descriptor);
	descriptor->release();
	return {
		.structureSize = sizes.accelerationStructureSize,
		.buildScratchSize = scratchBefore + sizes.buildScratchBufferSize,
		.updateScratchSize = scratchBefore + sizes.refitScratchBufferSize,
	};
}

template <>
void CommandEncoder<kMetal>::BuildAccelerationStructure(
	const AccelerationStructure<kMetal>& structure,
	std::span<const AccelerationStructureGeometry> geometries,
	AccelerationStructureBuildFlags flags,
	uint64_t scratchAddress,
	bool update) const
{
	ZoneScopedN("CommandEncoder::BuildAccelerationStructure");

	using namespace accelerationstructure;

	AccelerationStructureHandle<kMetal> handle = structure;
	auto [scratch, scratchOffset] = metal::FindBuffer(scratchAddress);
	ENSUREF(scratchOffset % kScratchAlignment == 0, "an acceleration structure build's scratch at offset {} (256 bytes aligned)", scratchOffset);
	MTL::AccelerationStructureDescriptor* descriptor = nullptr;
	const bool topLevel = structure.GetDesc().type == AccelerationStructureType::kTopLevel;
	if (topLevel)
	{
		ENSUREF(geometries.size() == 1, "a top level structure of {} sets of instances (one)", geometries.size());
		const auto* instances = std::get_if<AccelerationStructureInstances>(geometries.data());
		ENSUREF(instances != nullptr, "a top level structure of triangles");
		const auto layout = TopLevelLayout(instances->count);

		// the instances converted on the compute encoder (the next dispatch sets its own state again: PrepareDispatch)
		auto* pipeline = InstancePipeline(*structure.GetDevice().GetAllocator());
		const auto [source, sourceOffset] = metal::FindBuffer(instances->address);
		auto* compute = myCmd->ComputeEncoder();
		compute->setComputePipelineState(pipeline);
		compute->setBuffer(source, sourceOffset, 0);
		compute->setBuffer(scratch, scratchOffset + layout.instances, 1);
		compute->setBuffer(scratch, scratchOffset + layout.count, 2);
		const uint32_t count = instances->count;
		compute->setBytes(&count, sizeof(count), 3);
		compute->dispatchThreads(MTL::Size::Make(1, 1, 1), MTL::Size::Make(1, 1, 1));

		descriptor = NewTopLevel(instances->count, flags, scratch, scratchOffset);
		scratchOffset += layout.metal;
	}
	else
		descriptor = NewBottomLevel(geometries, flags);

	// Metal's encoder of its own: hazard tracking orders it after the conversion and the builds before it, and the bottom
	// levels a top level's instances name declared used (see metal::UseBottomLevels)
	auto* encoder = myCmd->AccelerationStructureEncoder();
	if (topLevel)
		metal::UseBottomLevels(encoder);
	if (update)
		encoder->refitAccelerationStructure(handle, descriptor, nullptr, scratch, scratchOffset);
	else
		encoder->buildAccelerationStructure(handle, descriptor, scratch, scratchOffset);
	descriptor->release();
}

template <>
void CommandEncoder<kMetal>::CopyAccelerationStructure(
	const AccelerationStructure<kMetal>& source, const AccelerationStructure<kMetal>& destination, bool compact) const
{
	AccelerationStructureHandle<kMetal> from = source;
	AccelerationStructureHandle<kMetal> to = destination;
	auto* encoder = myCmd->AccelerationStructureEncoder();
	if (compact)
		encoder->copyAndCompactAccelerationStructure(from, to);
	else
		encoder->copyAccelerationStructure(from, to);
}

} // namespace rhi
