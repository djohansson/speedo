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
// BVHs (see gfx/bvh.h): a bottom level structure of what doesn't move, built once of the model's own indices and
// vertices, a geometry per triangle list and instance placed by the instance's transform (bvh::RayQueryGeometries); one
// of what moves per frame index, refit each frame to where it was placed (bvh::Moving::WriteRayQueryCorners); and a top
// level structure per frame index of their two instances (what doesn't move, BVH_RAY_QUERY_MASK_STATIC, then what does,
// BVH_RAY_QUERY_MASK_MOVING; an instance of nothing is inactive), which the shaders bind as gBvhScene[frame]. the alpha
// masked geometries aren't opaque: their hits are candidates the shaders test. what moves' first geometry among all is
// its instance's custom index (what doesn't move's is 0), which the shaders' table of geometries is indexed by
class AccelerationStructures
{
public:
	// a geometry (see bvh::RayQueryGeometry): its first index (what doesn't move) or corner (what moves), its triangles,
	// and whether it is opaque (not alpha masked)
	struct Geometry
	{
		uint32_t first = 0;
		uint32_t triangleCount = 0;
		bool opaque = true;
	};

	// what they are built of, in buffers with BufferUsage::kAccelerationStructureInput and kDeviceAddress: what doesn't
	// move, the model's vertices (a position at the start of each, vertexStride apart), its indices and a transform per
	// geometry (3x4 floats, row major); what moves, a frame index's corners (a float4 each, three a triangle). none of
	// either: no structure of it
	struct Inputs
	{
		const Buffer* vertices = nullptr;
		uint32_t vertexStride = 0;
		uint32_t vertexCount = 0;
		const Buffer* indices = nullptr;
		const Buffer* transforms = nullptr;
		std::vector<Geometry> staticGeometries;
		std::span<const Buffer> movingCorners;
		std::vector<Geometry> movingGeometries;
	};

	AccelerationStructures(Device& device, std::string_view name, Inputs&& inputs);
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
	// the geometries of what doesn't move, and of what moves as a frame index has it
	[[nodiscard]] std::vector<rhi::AccelerationStructureGeometry> StaticGeometries() const;
	[[nodiscard]] std::vector<rhi::AccelerationStructureGeometry> MovingGeometries(uint32_t frameIndex) const;

	Device& myDevice;
	Inputs myInputs;
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
