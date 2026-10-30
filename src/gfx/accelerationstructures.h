#pragma once

#include <gfx/gpu.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <gfx/shaders/capi.h>

namespace gfx
{

// what the device's ray queries trace of a model (DeviceLimits::rayQuery, BVH_RAY_QUERY in the shaders), in place of its
// BVHs (see gfx/bvh.h): a bottom level structure of what doesn't move, built once of its triangles
// (bvh::RayQueryTriangles); one of what moves per frame index, refit each frame to where it was placed
// (bvh::Moving::WriteRayQueryTriangles); and a top level structure per frame index of their two instances (what doesn't
// move, BVH_RAY_QUERY_MASK_STATIC, then what does, BVH_RAY_QUERY_MASK_MOVING; an instance of nothing is inactive), which
// the shaders bind as gBvhScene[frame]. each bottom level structure is its opaque triangles, then its alpha masked ones,
// a geometry of their own whose candidates the shaders test (their instance's custom index is where they start)
class AccelerationStructures
{
public:
	// the triangles (BVH_RAY_QUERY_TRIANGLE_FLOAT4_COUNT float4 each, with BufferUsage::kAccelerationStructureInput and
	// kDeviceAddress): what doesn't move in staticTriangles (null if nothing), what moves in movingTriangles, a buffer per
	// frame index (empty if nothing does), each with its count and how many of them are opaque
	struct Triangles
	{
		const Buffer* staticTriangles = nullptr;
		uint32_t staticCount = 0;
		uint32_t staticOpaqueCount = 0;
		std::span<const Buffer> movingTriangles;
		uint32_t movingCount = 0;
		uint32_t movingOpaqueCount = 0;
	};

	AccelerationStructures(Device& device, std::string_view name, const Triangles& triangles);
	~AccelerationStructures();
	AccelerationStructures(const AccelerationStructures&) = delete;
	AccelerationStructures& operator=(const AccelerationStructures&) = delete;

	// records building all of them (once, after the triangles' uploads are acquired), ending with a barrier to the ray
	// queries after. returns the scratch memory the build takes, to retire once the gpu is done with it
	[[nodiscard]] std::shared_ptr<Buffer> RecordBuild(CommandBufferHandle cmd);

	// records refitting the frame index's moving structure to its triangles as they are now (written by the host since
	// the frame index's last frame completed), and building its top level structure again, ending with a barrier to the
	// ray queries after. nothing if nothing moves
	void RecordUpdate(CommandBufferHandle cmd, uint32_t frameIndex);

	// what doesn't move is built to be compacted: once the build is done (its compacted size queried, which CanCompact
	// reads without waiting), RecordCompaction records copying it into a structure of that size, the instances naming
	// that one and every top level structure built again (in order with the frames: a submission after them), and returns
	// what it replaced, to retire once the gpu is done with it
	[[nodiscard]] bool CanCompact();
	[[nodiscard]] std::shared_ptr<void> RecordCompaction(CommandBufferHandle cmd);

	[[nodiscard]] const AccelerationStructure& GetTopLevel(uint32_t frameIndex) const noexcept { return myTopLevels[frameIndex]; }

private:
	// records building the frame index's top level structure, of its instances
	void RecordTopLevelBuild(const CommandEncoder& encoder, uint32_t frameIndex);
	// what a bottom level structure's triangles are, as geometries: the opaque ones, then the alpha masked
	[[nodiscard]] static std::vector<rhi::AccelerationStructureGeometry> Geometries(const Buffer& triangles, uint32_t count, uint32_t opaqueCount);

	Device& myDevice;
	Triangles myTriangles;
	std::optional<AccelerationStructure> myStatic;
	std::optional<QueryPool> myCompactedSizeQuery; // what doesn't move's, until it is compacted
	uint64_t myCompactedSize = 0;
	std::vector<AccelerationStructure> myMoving; // per frame index
	std::vector<AccelerationStructure> myTopLevels; // per frame index
	std::vector<Buffer> myInstances; // per frame index, host visible: two rhi::AccelerationStructureInstance
	std::vector<Buffer> myScratch; // per frame index: the moving structure's update, then the top level build
	rhi::AccelerationStructureSizes myStaticSizes{};
	rhi::AccelerationStructureSizes myMovingSizes{};
	rhi::AccelerationStructureSizes myTopLevelSizes{};
	uint64_t myTopLevelScratchOffset = 0; // in each frame index's scratch, past the moving structure's update
};

} // namespace gfx
