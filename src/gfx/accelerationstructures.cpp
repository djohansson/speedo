#include "accelerationstructures.h"

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <print>
#include <utility>

namespace gfx
{

using namespace rhi;

namespace accelerationstructures
{

// address rounded up to alignment (a power of two, or 0 for none)
[[nodiscard]] static uint64_t AlignUp(uint64_t address, uint64_t alignment)
{
	return alignment > 0 ? (address + alignment - 1) & ~(alignment - 1) : address;
}

// a build's scratch memory of size bytes, which may start past its address to be aligned (see ScratchAddress)
[[nodiscard]] static Buffer CreateScratch(Device& device, std::string_view name, uint64_t size)
{
	return Buffer(BufferCreateDesc{
		device.CreateDeviceObjectCreateDesc(name),
		std::max<uint64_t>(size + device.GetLimits().accelerationStructureScratchAlignment, 1),
		BufferUsage::kStorage | BufferUsage::kDeviceAddress,
		MemoryProperty::kDeviceLocal});
}

[[nodiscard]] static uint64_t ScratchAddress(const Device& device, const Buffer& scratch, uint64_t offset = 0)
{
	return AlignUp(scratch.GetDeviceAddress() + offset, device.GetLimits().accelerationStructureScratchAlignment);
}

// what doesn't move: traced most, built once, then compacted (see RecordCompaction)
constexpr AccelerationStructureBuildFlags kStaticFlags =
	AccelerationStructureBuildFlags::kPreferFastTrace | AccelerationStructureBuildFlags::kAllowCompaction;

// a build's writes, read by the next build (a top level structure reads its bottom level ones), or by ray queries
static void BuildBarrier(const CommandEncoder& encoder)
{
	encoder.Barrier(
		PipelineStage::kAccelerationStructureBuild,
		Access::kAccelerationStructureWrite,
		PipelineStage::kAccelerationStructureBuild | PipelineStage::kAllCommands,
		Access::kAccelerationStructureRead);
}

} // namespace accelerationstructures

std::vector<AccelerationStructureGeometry> AccelerationStructures::Geometries(const Buffer& triangles, uint32_t count, uint32_t opaqueCount)
{
	constexpr uint32_t kStride = 4 * sizeof(float); // a corner a float4
	static_assert(BVH_RAY_QUERY_TRIANGLE_FLOAT4_COUNT == 3);
	std::vector<AccelerationStructureGeometry> geometries;
	uint64_t address = triangles.GetDeviceAddress();
	if (opaqueCount > 0)
		geometries.emplace_back(AccelerationStructureTriangles{
			.vertexAddress = address, .vertexStride = kStride, .triangleCount = opaqueCount, .opaque = true});
	if (count > opaqueCount)
		geometries.emplace_back(AccelerationStructureTriangles{
			.vertexAddress = address + (static_cast<uint64_t>(opaqueCount) * 3 * kStride),
			.vertexStride = kStride,
			.triangleCount = count - opaqueCount,
			.opaque = false});
	return geometries;
}

AccelerationStructures::AccelerationStructures(Device& device, std::string_view name, const Triangles& triangles)
	: myDevice(device)
	, myTriangles(triangles)
{
	ZoneScopedN("gfx::AccelerationStructures");

	using namespace accelerationstructures;

	// an instance's custom index (where its alpha masked triangles start) has 24 bits
	constexpr uint32_t kCustomIndexCount = 1U << 24U;
	ENSUREF(
		triangles.staticOpaqueCount < kCustomIndexCount && triangles.movingOpaqueCount < kCustomIndexCount,
		"{}: more opaque triangles than a ray query instance's custom index holds ({})", name, kCustomIndexCount);

	if (triangles.staticTriangles != nullptr && triangles.staticCount > 0)
	{
		auto geometries = Geometries(*triangles.staticTriangles, triangles.staticCount, triangles.staticOpaqueCount);
		myStaticSizes = AccelerationStructure::GetSizes(device, AccelerationStructureType::kBottomLevel, geometries, kStaticFlags);
		myStatic.emplace(AccelerationStructureCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("{} (static structure)", name)), AccelerationStructureType::kBottomLevel,
			myStaticSizes.structureSize});
	}
	bool moves = !triangles.movingTriangles.empty() && triangles.movingCount > 0;
	if (moves)
	{
		auto geometries = Geometries(triangles.movingTriangles.front(), triangles.movingCount, triangles.movingOpaqueCount);
		myMovingSizes = AccelerationStructure::GetSizes(
			device, AccelerationStructureType::kBottomLevel, geometries,
			AccelerationStructureBuildFlags::kPreferFastBuild | AccelerationStructureBuildFlags::kAllowUpdate);
	}
	std::array<AccelerationStructureGeometry, 1> instanceGeometry{AccelerationStructureInstances{.address = 0, .count = 2}};
	myTopLevelSizes = AccelerationStructure::GetSizes(device, AccelerationStructureType::kTopLevel, instanceGeometry, AccelerationStructureBuildFlags::kPreferFastTrace);
	myTopLevelScratchOffset = AlignUp(std::max(myMovingSizes.buildScratchSize, myMovingSizes.updateScratchSize), device.GetLimits().accelerationStructureScratchAlignment);

	if (triangles.staticCount + triangles.movingCount > 0)
		std::println(
			"{}: ray query structures of {} triangles that don't move ({} alpha masked, {:.1f} MB) and {} that do ({} alpha masked, {:.1f} MB a frame)",
			name, triangles.staticCount, triangles.staticCount - triangles.staticOpaqueCount, static_cast<double>(myStaticSizes.structureSize) / (1 << 20),
			triangles.movingCount, triangles.movingCount - triangles.movingOpaqueCount, static_cast<double>(myMovingSizes.structureSize) / (1 << 20));

	for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
	{
		if (moves)
			myMoving.emplace_back(AccelerationStructureCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("{} (moving structure {})", name, frameIt)), AccelerationStructureType::kBottomLevel,
				myMovingSizes.structureSize});
		myTopLevels.emplace_back(AccelerationStructureCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("{} (top level structure {})", name, frameIt)), AccelerationStructureType::kTopLevel,
			myTopLevelSizes.structureSize});
		myScratch.push_back(CreateScratch(
			device, std::format("{} (structures' scratch {})", name, frameIt), myTopLevelScratchOffset + myTopLevelSizes.buildScratchSize));

		// what doesn't move, and what moves as the frame index's structure has it (the shaders tell them apart by their index)
		std::array<AccelerationStructureInstance, 2> instances{};
		instances[0].customIndexAndMask = triangles.staticOpaqueCount | (BVH_RAY_QUERY_MASK_STATIC << 24U);
		instances[0].structureAddress = myStatic ? myStatic->GetDeviceAddress() : 0;
		static_assert(BVH_RAY_QUERY_INSTANCE_MOVING == 1);
		instances[1].customIndexAndMask = triangles.movingOpaqueCount | (BVH_RAY_QUERY_MASK_MOVING << 24U);
		instances[1].structureAddress = moves ? myMoving.back().GetDeviceAddress() : 0;
		auto& instanceBuffer = myInstances.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("{} (instances {})", name, frameIt)),
			sizeof(instances),
			BufferUsage::kAccelerationStructureInput | BufferUsage::kDeviceAddress | BufferUsage::kTransferDestination, // see RecordCompaction
			MemoryProperty::kHostVisible});
		auto memory = instanceBuffer.Map();
		std::memcpy(memory.data(), instances.data(), sizeof(instances));
		instanceBuffer.Flush(0, sizeof(instances));
		instanceBuffer.Unmap();
	}
}

AccelerationStructures::~AccelerationStructures() = default;

std::shared_ptr<Buffer> AccelerationStructures::RecordBuild(CommandBufferHandle cmd)
{
	ZoneScopedN("gfx::AccelerationStructures::RecordBuild");

	using namespace accelerationstructures;

	CommandEncoder encoder(cmd);
	std::shared_ptr<Buffer> scratch;
	if (myStatic)
	{
		scratch = std::make_shared<Buffer>(CreateScratch(myDevice, std::format("{} scratch", myStatic->GetName()), myStaticSizes.buildScratchSize));
		myCompactedSizeQuery.emplace(QueryPoolCreateDesc{
			myDevice.CreateDeviceObjectCreateDesc(std::format("{} compacted size", myStatic->GetName())), 1, rhi::QueryType::kAccelerationStructureCompactedSize});
		myCompactedSizeQuery->Reset(cmd, 0, 1);
		encoder.BuildAccelerationStructure(
			*myStatic,
			Geometries(*myTriangles.staticTriangles, myTriangles.staticCount, myTriangles.staticOpaqueCount),
			kStaticFlags,
			ScratchAddress(myDevice, *scratch));
	}
	for (uint32_t frameIt = 0; frameIt < myMoving.size(); frameIt++)
		encoder.BuildAccelerationStructure(
			myMoving[frameIt],
			Geometries(myTriangles.movingTriangles[frameIt], myTriangles.movingCount, myTriangles.movingOpaqueCount),
			AccelerationStructureBuildFlags::kPreferFastBuild | AccelerationStructureBuildFlags::kAllowUpdate,
			ScratchAddress(myDevice, myScratch[frameIt]));
	BuildBarrier(encoder);
	if (myCompactedSizeQuery)
		encoder.WriteAccelerationStructureCompactedSize(*myStatic, *myCompactedSizeQuery, 0);
	for (uint32_t frameIt = 0; frameIt < myTopLevels.size(); frameIt++)
		RecordTopLevelBuild(encoder, frameIt);
	BuildBarrier(encoder);
	return scratch;
}

void AccelerationStructures::RecordUpdate(CommandBufferHandle cmd, uint32_t frameIndex)
{
	if (myMoving.empty())
		return;

	ZoneScopedN("gfx::AccelerationStructures::RecordUpdate");

	using namespace accelerationstructures;

	CommandEncoder encoder(cmd);
	encoder.BuildAccelerationStructure(
		myMoving[frameIndex],
		Geometries(myTriangles.movingTriangles[frameIndex], myTriangles.movingCount, myTriangles.movingOpaqueCount),
		AccelerationStructureBuildFlags::kPreferFastBuild | AccelerationStructureBuildFlags::kAllowUpdate,
		ScratchAddress(myDevice, myScratch[frameIndex]),
		true);
	BuildBarrier(encoder);
	RecordTopLevelBuild(encoder, frameIndex);
	BuildBarrier(encoder);
}

void AccelerationStructures::RecordTopLevelBuild(const CommandEncoder& encoder, uint32_t frameIndex)
{
	std::array<AccelerationStructureGeometry, 1> instances{
		AccelerationStructureInstances{.address = myInstances[frameIndex].GetDeviceAddress(), .count = 2}};
	encoder.BuildAccelerationStructure(
		myTopLevels[frameIndex], instances, AccelerationStructureBuildFlags::kPreferFastTrace,
		accelerationstructures::ScratchAddress(myDevice, myScratch[frameIndex], myTopLevelScratchOffset));
}

bool AccelerationStructures::CanCompact()
{
	if (!myCompactedSizeQuery)
		return false;
	std::array<std::optional<uint64_t>, 1> size;
	myCompactedSizeQuery->Read(0, size);
	myCompactedSize = size[0].value_or(0);
	return myCompactedSize > 0;
}

std::shared_ptr<void> AccelerationStructures::RecordCompaction(CommandBufferHandle cmd)
{
	ZoneScopedN("gfx::AccelerationStructures::RecordCompaction");

	using namespace accelerationstructures;

	ENSURE(myCompactedSizeQuery && myCompactedSize > 0);
	std::println(
		"{}: compacted from {:.1f} MB to {:.1f} MB", myStatic->GetName(), static_cast<double>(myStaticSizes.structureSize) / (1 << 20),
		static_cast<double>(myCompactedSize) / (1 << 20));
	AccelerationStructure compacted(AccelerationStructureCreateDesc{
		myDevice.CreateDeviceObjectCreateDesc(myStatic->GetName()), AccelerationStructureType::kBottomLevel, myCompactedSize});
	CommandEncoder encoder(cmd);
	encoder.CopyAccelerationStructure(*myStatic, compacted, true);
	BuildBarrier(encoder);
	// the instances name it from now on: written in order with the frames, which build top level structures from them
	uint64_t address = compacted.GetDeviceAddress();
	for (const auto& instances : myInstances)
		encoder.UpdateBuffer(instances, offsetof(AccelerationStructureInstance, structureAddress), std::as_bytes(std::span(&address, 1)));
	encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kAccelerationStructureBuild, Access::kShaderRead);
	for (uint32_t frameIt = 0; frameIt < myTopLevels.size(); frameIt++)
		RecordTopLevelBuild(encoder, frameIt);
	BuildBarrier(encoder);
	auto previous = std::make_shared<std::pair<AccelerationStructure, QueryPool>>(std::move(*myStatic), std::move(*myCompactedSizeQuery));
	myStatic = std::move(compacted);
	myCompactedSizeQuery.reset();
	return previous;
}

} // namespace gfx
