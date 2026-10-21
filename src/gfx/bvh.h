#pragma once

#include <gfx/sceneanimation.h>
#include <gfx/shaders/capi.h>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace core
{
class TaskExecutor;
}

namespace gfx
{
struct ModelDesc;
}

// a ray tracing structure over a model's triangles, which the indirect lighting's rays traverse in compute (see
// gfx/shaders/bvh.slang and GiData::trace): tinybvh's compressed wide BVH (CWBVH, Ylitie et al. 2017), built on the cpu
namespace gfx::bvh
{

// a CWBVH as the shaders read it (gBvhNodes, gBvhTriangles): nodes of 5 float4 (BVH_NODE_FLOAT4_COUNT), and
// triangles of 4 (BVH_TRIANGLE_FLOAT4_COUNT): Baldwin and Weber's transform to the triangle's space (3 rows), then x the
// sign of the normal's largest component (which the transform divides by, so its rows lose which way the triangle
// faces), y its BVH_TRIANGLE_FLAG_* bits (and material), z its first index and w its index among the triangles built,
// as float bits. empty if there were
// no triangles
struct Data
{
	std::vector<glm::vec4> nodes;
	std::vector<glm::vec4> triangles;
	uint32_t triangleCount = 0;
};

// a triangle to build over: its corners (world space), its BVH_TRIANGLE_FLAG_* bits (and material, see
// BVH_TRIANGLE_MATERIAL_SHIFT) and its first index in the model's index buffer
struct Triangle
{
	std::array<glm::vec3, 3> corners{};
	uint32_t flags = 0;
	uint32_t firstIndex = 0;
};

// what of a model the BVH holds, what the voxels hold of its static draws (see ComputeGiVoxelize): its triangle lists'
// triangles that don't move and aren't blended or transmissive, at each of their instances
[[nodiscard]] std::vector<Triangle> Triangles(
	const ModelDesc& desc, std::span<const uint32_t> indices, std::span<const VertexP3fN3fTa4fT014fC4f> vertices);

// a CWBVH over triangles, built on the executor's threads
[[nodiscard]] Data Build(std::span<const Triangle> triangles, core::TaskExecutor& executor);

// the same in the moving BVH's layout (tinybvh's BVH_GPU, see Moving; BVH2_NODE_FLOAT4_COUNT and
// BVH2_TRIANGLE_FLOAT4_COUNT), for assettest's check of its traversal (BvhTraverse2)
[[nodiscard]] Data BuildBinary(std::span<const Triangle> triangles, core::TaskExecutor& executor);

// a BVH of what of a model moves (skinned, morphed, or following animated nodes: ModelSubmesh::moves), which the static
// one leaves out: its triangle lists' triangles at each of their instances, in world space, placed each frame as the
// vertex shader places them (morphed, skinned, then by the instance, see WorldPosition) and the BVH refit to them (its
// tree is built once, where they are first placed), in tinybvh's binary GPU layout (BVH_GPU: copied from the refit tree,
// where encoding a CWBVH again took BrainStem's 61000 triangles 3 ms a frame). one BVH rather than one per submesh: a ray
// then traverses it once, where with a BVH each BrainStem's dozens of overlapping parts each took one. the shaders read
// it from a frame's buffers (gBvhMovingNodes, gBvhMovingTriangles)
class Moving
{
public:
	Moving(
		const ModelDesc& desc,
		std::span<const uint32_t> indices,
		std::span<const VertexP3fN3fTa4fT014fC4f> vertices,
		std::span<const SkinVertex> skinVertices,
		std::span<const MorphDelta> morphDeltas,
		core::TaskExecutor& executor);
	~Moving();
	Moving(const Moving&) = delete;
	Moving& operator=(const Moving&) = delete;

	[[nodiscard]] bool Empty() const noexcept;
	// the float4 counts of its nodes and triangles: the same every frame
	[[nodiscard]] size_t NodeFloat4Count() const noexcept;
	[[nodiscard]] size_t TriangleFloat4Count() const noexcept;

	// places the triangles where a frame's joint matrices (mesh to world), morph weights and instances put them (on the
	// executor's threads), and fits the BVH to them: the first time builds its tree (the counts above are known from
	// then on), later times refit it
	void Place(std::span<const SceneMatrix> joints, std::span<const float> morphWeights, std::span<const ModelInstance> instances);

	// writes the BVH as placed last to a frame's buffers' memory (NodeFloat4Count and TriangleFloat4Count float4s)
	void Write(std::span<std::byte> nodes, std::span<std::byte> triangles) const;

private:
	struct Impl;
	std::unique_ptr<Impl> myImpl;
};

// a tag of what Build makes (tinybvh's version, and ours), for the caches that keep it (the model's, see Model::Load)
[[nodiscard]] std::string Version();

} // namespace gfx::bvh
