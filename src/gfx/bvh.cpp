#include "bvh.h"

#include <gfx/model.h>

#include <core/assert.h>
#include <core/profiling.h>
#include <core/taskexecutor.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>
#include <vector>

#include <glm/gtc/type_ptr.hpp>

// the builds run on the application's executor (see detail::Executor), not on tinybvh's own threads
#define TINYBVH_NO_BUILTIN_POOL
// the depth its builds and conversions walk (no traversal runs on the cpu here): a polygon of hundreds of vertices is a fan
// of slivers around one corner, which the builders peel off a few at a time (geodesic.zip's duals, faces of up to 384
// vertices, went past tinybvh's 128: "CombineLeafs, tree too deep")
#define TINYBVH_STACK_SIZE 1024
#define TINYBVH_IMPLEMENTATION
#include <tinybvh/tiny_bvh.h>

// bvh.slang reads the compressed triangles (4 float4: Baldwin and Weber's transform, then the index), which tiny_bvh.h
// selects at the commit the port pins; its dev branch stores them uncompressed (3 float4)
#ifndef CWBVH_COMPRESSED_TRIS
#error "gfx/shaders/bvh.slang reads tinybvh's compressed CWBVH triangles"
#endif

namespace gfx::bvh
{

// the most triangles a leaf of the binary layout holds: BVH_GPU's leaf references have 7 bits for the count (tinybvh's
// CompactLeafRef clamps it, and the rest of the leaf was never tested), and a fan of coplanar slivers the builders can't
// split leaves more (geodesic.zip's duals); the builders' leaves are smaller than this anyway
static constexpr uint32_t kBinaryLeafTriangleCount = 16;

namespace detail
{

// tinybvh's build hooks over a TaskExecutor: spawned work is a task each, which a barrier joins (a spawned task may spawn
// more, so it joins until none are left), and parallel loops are ForEach's
struct Executor
{
	core::TaskExecutor& executor;
	std::mutex mutex;
	std::vector<core::Future<void>> spawned;
};

static void Spawn(void (*fn)(void* payload), const void* payload, uint32_t payloadSize, void* userdata)
{
	auto& context = *static_cast<Executor*>(userdata);
	std::vector<std::byte> copy(payloadSize);
	std::memcpy(copy.data(), payload, payloadSize);
	auto [task, future] = core::CreateTask([fn, copy = std::move(copy)]() mutable { fn(copy.data()); });
	{
		std::lock_guard lock(context.mutex);
		context.spawned.push_back(std::move(future));
	}
	context.executor.Submit({&task, 1});
}

static void Barrier(void* userdata)
{
	auto& context = *static_cast<Executor*>(userdata);
	for (;;)
	{
		std::vector<core::Future<void>> spawned;
		{
			std::lock_guard lock(context.mutex);
			spawned.swap(context.spawned);
		}
		if (spawned.empty())
			return;
		for (auto& future : spawned)
			context.executor.Join(std::move(future));
	}
}

static void ParallelFor(uint32_t count, void (*fn)(uint32_t index, void* payload), void* payload, void* userdata)
{
	auto& context = *static_cast<Executor*>(userdata);
	context.executor.ForEach(count, [fn, payload](size_t index) { fn(static_cast<uint32_t>(index), payload); });
}

static void FatalError(const char* message)
{
	ENSUREF(false, "tinybvh: {}", message);
}

// the binary layout's triangles in its leaves' order (BVH2_TRIANGLE_FLOAT4_COUNT each: the first corner, w its index, then
// the edges from it): tinybvh's, except where the whole tree is one leaf, which BVH_GPU::ConvertFrom wraps in a root
// without ordering them (orderedVerts null: a mesh of one triangle, the moving BVH of AnimatedTriangle); those in the tree's
// primitive order, as it would
static void CopyOrderedTriangles(
	const tinybvh::BVH& tree, const tinybvh::BVH_GPU& layout, std::span<const tinybvh::bvhvec4> corners, glm::vec4* triangles)
{
	if (layout.orderedVerts.data != nullptr)
	{
		std::memcpy(triangles, layout.orderedVerts.data, static_cast<size_t>(layout.idxCount) * BVH2_TRIANGLE_FLOAT4_COUNT * sizeof(glm::vec4));
		return;
	}
	for (uint32_t indexIt = 0; indexIt < layout.idxCount; indexIt++)
	{
		uint32_t primitive = tree.primIdx[indexIt];
		const auto& v0 = corners[static_cast<size_t>(primitive) * 3];
		const auto& v1 = corners[(static_cast<size_t>(primitive) * 3) + 1];
		const auto& v2 = corners[(static_cast<size_t>(primitive) * 3) + 2];
		triangles[(indexIt * 3) + 0] = glm::vec4(v0.x, v0.y, v0.z, std::bit_cast<float>(primitive));
		triangles[(indexIt * 3) + 1] = glm::vec4(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z, 0.0F);
		triangles[(indexIt * 3) + 2] = glm::vec4(v2.x - v0.x, v2.y - v0.y, v2.z - v0.z, 0.0F);
	}
}

// which way a triangle faces, as the CWBVH's triangle transform loses it: the sign of its normal's largest component (the
// one tinybvh's PrecomputeTriangle divides by, picked the same way)
static float DominantSign(const glm::vec3& normal)
{
	glm::vec3 size = glm::abs(normal);
	float dominant = size.x > size.y && size.x > size.z ? normal.x : (size.y > size.z ? normal.y : normal.z);
	return dominant < 0.0F ? -1.0F : 1.0F;
}

// a triangle's flags (BVH_TRIANGLE_FLAG_*) and its material's index in gMaterialData (the model's materials are 1 and up,
// 0 the default's, for none), which a ray reads of what it meets: its alpha mask (cut out as the main pass's alpha test
// does, by the base color's or the mask's texture), and its emission (see GiHitRadiance: only the alpha masked triangles
// carried it, and every other one met had the default material's, black, so an emitter gave the rays nothing)
static uint32_t TriangleFlags(const ModelSubmesh& submesh, const ModelMaterial* material)
{
	ENSUREF(
		static_cast<uint64_t>(submesh.material + 1) < (1ULL << (32U - BVH_TRIANGLE_MATERIAL_SHIFT)),
		"a BVH triangle's flags hold material indices below {}, not {}", 1ULL << (32U - BVH_TRIANGLE_MATERIAL_SHIFT), submesh.material + 1);
	uint32_t flags = static_cast<uint32_t>(submesh.material + 1) << BVH_TRIANGLE_MATERIAL_SHIFT;
	if (material != nullptr && material->doubleSided)
		flags |= BVH_TRIANGLE_FLAG_DOUBLE_SIDED;
	if (material != nullptr && material->alphaCutoff > 0.0F && (!material->diffuseTexture.empty() || !material->alphaTexture.empty()))
		flags |= BVH_TRIANGLE_FLAG_ALPHA_MASKED;
	return flags;
}

} // namespace detail

std::vector<Triangle> Triangles(
	const ModelDesc& desc, std::span<const uint32_t> indices, std::span<const VertexP3fN3fTa4fT014fC4f> vertices)
{
	std::vector<Triangle> triangles;
	for (const auto& submesh : desc.submeshes)
	{
		if (submesh.moves || submesh.topology != rhi::PrimitiveTopology::kTriangleList)
			continue;
		const ModelMaterial* material = submesh.material >= 0 ? &desc.materials[static_cast<size_t>(submesh.material)] : nullptr;
		if (material != nullptr && (material->blend || material->transmission > 0.0F))
			continue;
		uint32_t flags = detail::TriangleFlags(submesh, material);
		for (uint32_t instanceIt = 0; instanceIt < submesh.instanceCount; instanceIt++)
		{
			const glm::mat4& transform = desc.instances[submesh.firstInstance + instanceIt];
			for (uint32_t indexIt = 0; indexIt + 2 < submesh.indexCount; indexIt += 3)
			{
				Triangle& triangle = triangles.emplace_back(Triangle{.flags = flags, .firstIndex = submesh.firstIndex + indexIt});
				for (uint32_t cornerIt = 0; cornerIt < 3; cornerIt++)
				{
					const auto& position = vertices[indices[submesh.firstIndex + indexIt + cornerIt]].position;
					triangle.corners[cornerIt] = glm::vec3(transform * glm::vec4(position[0], position[1], position[2], 1.0F));
				}
			}
		}
	}
	return triangles;
}

namespace detail
{

// how the static BVH is built: with spatial splits (tinybvh's BuildHQ, an SBVH: a triangle spanning several nodes'
// space is referenced from each, so the nodes overlap less and rays visit fewer), or binned SAH alone
// (SPEEDO_BVH_BUILD=fast, tinybvh's Build, as it was)
[[nodiscard]] bool SpatialSplits()
{
	static const bool spatialSplits = []
	{
		const char* build = std::getenv("SPEEDO_BVH_BUILD");
		return build == nullptr || std::string_view(build) != "fast";
	}();
	return spatialSplits;
}

} // namespace detail

Data Build(std::span<const Triangle> triangles, core::TaskExecutor& executor)
{
	ZoneScopedN("gfx::bvh::Build");

	tinybvh::tinybvh_set_fatal_error_handler(detail::FatalError);

	Data data;
	data.triangleCount = static_cast<uint32_t>(triangles.size());
	if (data.triangleCount == 0)
		return data;

	// tinybvh's input: 3 corners a triangle
	std::vector<tinybvh::bvhvec4> corners;
	corners.reserve(triangles.size() * 3);
	for (const auto& triangle : triangles)
		for (const auto& corner : triangle.corners)
			corners.emplace_back(corner.x, corner.y, corner.z, 0.0F);

	detail::Executor context{.executor = executor};
	tinybvh::BVH8_CWBVH cwbvh(tinybvh::BVHContext{
		.spawn = detail::Spawn, .barrier = detail::Barrier, .parallel_for = detail::ParallelFor, .userdata = &context});
	if (detail::SpatialSplits())
		cwbvh.BuildHQ(corners.data(), data.triangleCount);
	else
		cwbvh.Build(corners.data(), data.triangleCount);

	data.nodes.resize(cwbvh.usedBlocks);
	std::memcpy(data.nodes.data(), cwbvh.bvh8Data, data.nodes.size() * sizeof(glm::vec4));
	data.triangles.resize(cwbvh.usedTriBlocks);
	std::memcpy(data.triangles.data(), cwbvh.bvh8Tris, data.triangles.size() * sizeof(glm::vec4));
	// each triangle's facing, flags and first index in its last float4's unused x, y and z
	ENSURE(data.triangles.size() % BVH_TRIANGLE_FLOAT4_COUNT == 0);
	for (size_t triangleIt = 0; triangleIt < data.triangles.size(); triangleIt += BVH_TRIANGLE_FLOAT4_COUNT)
	{
		auto& last = data.triangles[triangleIt + BVH_TRIANGLE_FLOAT4_COUNT - 1];
		auto primitive = std::bit_cast<uint32_t>(last.w);
		ENSURE(primitive < data.triangleCount);
		const auto& p = triangles[primitive].corners;
		last.x = detail::DominantSign(glm::cross(p[1] - p[0], p[2] - p[0]));
		last.y = std::bit_cast<float>(triangles[primitive].flags);
		last.z = std::bit_cast<float>(triangles[primitive].firstIndex);
	}
	return data;
}

// the moving BVH's triangles, by the submesh and instance they are of (each a run of the corners), placed last where
// (tinybvh's tree refits from them, so they stay put)
struct Moving::Impl
{
	struct Part
	{
		ModelSubmesh submesh;
		uint32_t instance = 0;
		uint32_t jointBase = SHADER_TYPES_NOT_SKINNED;
		uint32_t flags = 0;
		size_t firstCorner = 0;
	};

	std::unique_ptr<detail::Executor> context;
	std::vector<uint32_t> indices;
	std::vector<glm::vec3> positions; // the vertices' at rest
	std::vector<SkinVertex> skinVertices;
	std::vector<MorphDelta> morphDeltas;
	std::vector<Part> parts;
	std::vector<uint32_t> partOfTriangle;
	std::vector<tinybvh::bvhvec4> corners;
	std::unique_ptr<tinybvh::BVH> tree;
	std::unique_ptr<tinybvh::BVH_GPU> layout;
	bool buildsTree = true; // false: placed for the device's ray queries alone (see WriteRayQueryTriangles)
	uint32_t opaqueTriangleCount = 0; // the parts' triangles are the opaque ones first
};

Moving::Moving(
	const ModelDesc& desc,
	std::span<const uint32_t> indices,
	std::span<const VertexP3fN3fTa4fT014fC4f> vertices,
	std::span<const SkinVertex> skinVertices,
	std::span<const MorphDelta> morphDeltas,
	core::TaskExecutor& executor,
	bool tree)
	: myImpl(std::make_unique<Impl>())
{
	auto& impl = *myImpl;
	impl.context.reset(new detail::Executor{.executor = executor}); // NOLINT(cppcoreguidelines-owning-memory): an aggregate with a mutex
	size_t cornerCount = 0;
	impl.buildsTree = tree;
	// the opaque parts first, then the alpha masked (the ray queries' geometries, see RayQueryData)
	for (bool masked : {false, true})
	{
		for (const auto& submesh : desc.submeshes)
		{
			if (!submesh.moves || submesh.topology != rhi::PrimitiveTopology::kTriangleList || submesh.indexCount < 3)
				continue;
			const ModelMaterial* material = submesh.material >= 0 ? &desc.materials[static_cast<size_t>(submesh.material)] : nullptr;
			if (material != nullptr && (material->blend || material->transmission > 0.0F))
				continue;
			uint32_t flags = detail::TriangleFlags(submesh, material);
			if (((flags & BVH_TRIANGLE_FLAG_ALPHA_MASKED) != 0U) != masked)
				continue;
			uint32_t jointBase = submesh.skin >= 0 ? desc.animation.skins[static_cast<size_t>(submesh.skin)].jointBase : SHADER_TYPES_NOT_SKINNED;
			for (uint32_t instanceIt = 0; instanceIt < submesh.instanceCount; instanceIt++)
			{
				impl.parts.push_back(Impl::Part{
					.submesh = submesh,
					.instance = submesh.firstInstance + instanceIt,
					.jointBase = jointBase,
					.flags = flags,
					.firstCorner = cornerCount});
				cornerCount += submesh.indexCount / 3 * 3;
				impl.partOfTriangle.resize(cornerCount / 3, static_cast<uint32_t>(impl.parts.size() - 1));
			}
		}
		if (!masked)
			impl.opaqueTriangleCount = static_cast<uint32_t>(cornerCount / 3);
	}
	if (impl.parts.empty())
		return;
	impl.corners.resize(cornerCount);
	impl.indices.assign(indices.begin(), indices.end());
	impl.positions.reserve(vertices.size());
	for (const auto& vertex : vertices)
		impl.positions.emplace_back(vertex.position[0], vertex.position[1], vertex.position[2]);
	impl.skinVertices.assign(skinVertices.begin(), skinVertices.end());
	impl.morphDeltas.assign(morphDeltas.begin(), morphDeltas.end());
}

Moving::~Moving() = default;

bool Moving::Empty() const noexcept
{
	return myImpl->parts.empty();
}

size_t Moving::NodeFloat4Count() const noexcept
{
	return myImpl->layout ? static_cast<size_t>(myImpl->layout->usedNodes) * BVH2_NODE_FLOAT4_COUNT : 0;
}

size_t Moving::TriangleFloat4Count() const noexcept
{
	return myImpl->layout ? static_cast<size_t>(myImpl->layout->idxCount) * BVH2_TRIANGLE_FLOAT4_COUNT : 0;
}

void Moving::Place(std::span<const SceneMatrix> joints, std::span<const float> morphWeights, std::span<const ModelInstance> instances)
{
	ZoneScopedN("gfx::bvh::Moving::Place");

	auto& impl = *myImpl;
	if (impl.parts.empty())
		return;
	impl.context->executor.ForEach(
		impl.parts.size(),
		[&](size_t partIt)
		{
			const auto& part = impl.parts[partIt];
			const auto& submesh = part.submesh;
			glm::mat4 transform = glm::make_mat4(&instances[part.instance].modelTransform[0][0]);
			// each corner as WorldPosition places its vertex: morphed, skinned, then by the instance
			for (size_t cornerIt = 0; cornerIt < submesh.indexCount / 3 * 3; cornerIt++)
			{
				uint32_t vertex = impl.indices[submesh.firstIndex + cornerIt];
				glm::vec3 position = impl.positions[vertex];
				if (submesh.morphTargetCount > 0)
				{
					size_t row = submesh.morphDeltaBase + static_cast<size_t>(vertex - submesh.morphFirstVertex) * submesh.morphTargetCount;
					for (uint32_t targetIt = 0; targetIt < submesh.morphTargetCount; targetIt++)
						if (float weight = morphWeights[submesh.morphWeightBase + targetIt]; weight != 0.0F)
							position += weight * glm::make_vec3(impl.morphDeltas[row + targetIt].position);
				}
				if (part.jointBase != SHADER_TYPES_NOT_SKINNED)
				{
					const auto& skin = impl.skinVertices[vertex];
					glm::mat4 matrix(0.0F);
					for (uint32_t jointIt = 0; jointIt < 4; jointIt++)
					{
						uint32_t shift = (jointIt % 2) * 16;
						uint32_t joint = (skin.joints[jointIt / 2] >> shift) & 0xFFFFU;
						float weight = static_cast<float>((skin.weights[jointIt / 2] >> shift) & 0xFFFFU) / 65535.0F;
						if (weight > 0.0F)
							matrix += weight * joints[part.jointBase + joint];
					}
					position = glm::vec3(matrix * glm::vec4(position, 1.0F));
				}
				position = glm::vec3(transform * glm::vec4(position, 1.0F));
				impl.corners[part.firstCorner + cornerIt] = tinybvh::bvhvec4(position.x, position.y, position.z, 0.0F);
			}
		});
	if (!impl.buildsTree)
		return;
	// the binary tree built the first time (and refit at once: the refit orders its nodes, which Bonsai's builder doesn't,
	// so the count is the refit one's from then on), refit since, and copied to the GPU layout
	if (!impl.tree)
	{
		impl.tree = std::make_unique<tinybvh::BVH>(tinybvh::BVHContext{
			.spawn = detail::Spawn, .barrier = detail::Barrier, .parallel_for = detail::ParallelFor, .userdata = impl.context.get()});
		impl.tree->Build(impl.corners.data(), static_cast<uint32_t>(impl.corners.size() / 3));
		impl.tree->SplitLeafs(kBinaryLeafTriangleCount);
		impl.tree->Refit();
		impl.layout = std::make_unique<tinybvh::BVH_GPU>();
		impl.layout->ConvertFrom(*impl.tree, true);
		return;
	}
	auto nodeCount = impl.layout->usedNodes;
	impl.tree->Refit();
	impl.layout->ConvertFrom(*impl.tree, true);
	ENSUREF(impl.layout->usedNodes == nodeCount, "the refit moving BVH changed its node count ({} to {})", nodeCount, impl.layout->usedNodes);
}

void Moving::Write(std::span<std::byte> nodes, std::span<std::byte> triangles) const
{
	ZoneScopedN("gfx::bvh::Moving::Write");

	const auto& impl = *myImpl;
	ENSURE(impl.layout && nodes.size() >= NodeFloat4Count() * sizeof(glm::vec4) && triangles.size() >= TriangleFloat4Count() * sizeof(glm::vec4));
	static_assert(sizeof(tinybvh::BVH_GPU::BVHNode) == BVH2_NODE_FLOAT4_COUNT * sizeof(glm::vec4));
	std::memcpy(nodes.data(), impl.layout->bvhNode, NodeFloat4Count() * sizeof(glm::vec4));
	auto* triangleData = reinterpret_cast<glm::vec4*>(triangles.data());
	detail::CopyOrderedTriangles(*impl.tree, *impl.layout, impl.corners, triangleData);
	// each triangle's flags and first index in its edges' w (its corner's holds its index)
	for (size_t triangleIt = 0; triangleIt < TriangleFloat4Count(); triangleIt += BVH2_TRIANGLE_FLOAT4_COUNT)
	{
		auto primitive = std::bit_cast<uint32_t>(triangleData[triangleIt].w);
		const auto& part = impl.parts[impl.partOfTriangle[primitive]];
		triangleData[triangleIt + 1].w = std::bit_cast<float>(part.flags);
		triangleData[triangleIt + 2].w = std::bit_cast<float>(static_cast<uint32_t>(part.submesh.firstIndex + (primitive * 3 - part.firstCorner)));
	}
}

uint32_t Moving::TriangleCount() const noexcept
{
	return static_cast<uint32_t>(myImpl->corners.size() / 3);
}

uint32_t Moving::OpaqueTriangleCount() const noexcept
{
	return myImpl->opaqueTriangleCount;
}

void Moving::WriteRayQueryTriangles(std::span<std::byte> triangles) const
{
	ZoneScopedN("gfx::bvh::Moving::WriteRayQueryTriangles");

	const auto& impl = *myImpl;
	ENSURE(triangles.size() >= static_cast<size_t>(TriangleCount()) * BVH_RAY_QUERY_TRIANGLE_FLOAT4_COUNT * sizeof(glm::vec4));
	auto* data = reinterpret_cast<glm::vec4*>(triangles.data());
	impl.context->executor.ForEach(
		impl.parts.size(),
		[&](size_t partIt)
		{
			const auto& part = impl.parts[partIt];
			for (size_t cornerIt = 0; cornerIt < part.submesh.indexCount / 3 * 3; cornerIt++)
			{
				const auto& corner = impl.corners[part.firstCorner + cornerIt];
				uint32_t w = 0;
				if (cornerIt % 3 == 0)
					w = part.flags;
				else if (cornerIt % 3 == 1)
					w = static_cast<uint32_t>(part.submesh.firstIndex + cornerIt - 1);
				data[part.firstCorner + cornerIt] = glm::vec4(corner.x, corner.y, corner.z, std::bit_cast<float>(w));
			}
		});
}

Data BuildBinary(std::span<const Triangle> triangles, core::TaskExecutor& executor)
{
	ZoneScopedN("gfx::bvh::BuildBinary");

	Data data;
	data.triangleCount = static_cast<uint32_t>(triangles.size());
	if (data.triangleCount == 0)
		return data;
	std::vector<tinybvh::bvhvec4> corners;
	corners.reserve(triangles.size() * 3);
	for (const auto& triangle : triangles)
		for (const auto& corner : triangle.corners)
			corners.emplace_back(corner.x, corner.y, corner.z, 0.0F);
	detail::Executor context{.executor = executor};
	tinybvh::BVH tree(tinybvh::BVHContext{
		.spawn = detail::Spawn, .barrier = detail::Barrier, .parallel_for = detail::ParallelFor, .userdata = &context});
	tree.Build(corners.data(), data.triangleCount);
	tree.SplitLeafs(kBinaryLeafTriangleCount);
	tree.Refit();
	tinybvh::BVH_GPU layout;
	layout.ConvertFrom(tree, true);
	data.nodes.resize(static_cast<size_t>(layout.usedNodes) * BVH2_NODE_FLOAT4_COUNT);
	std::memcpy(data.nodes.data(), layout.bvhNode, data.nodes.size() * sizeof(glm::vec4));
	data.triangles.resize(static_cast<size_t>(layout.idxCount) * BVH2_TRIANGLE_FLOAT4_COUNT);
	detail::CopyOrderedTriangles(tree, layout, corners, data.triangles.data());
	for (size_t triangleIt = 0; triangleIt < data.triangles.size(); triangleIt += BVH2_TRIANGLE_FLOAT4_COUNT)
	{
		auto primitive = std::bit_cast<uint32_t>(data.triangles[triangleIt].w);
		data.triangles[triangleIt + 1].w = std::bit_cast<float>(triangles[primitive].flags);
		data.triangles[triangleIt + 2].w = std::bit_cast<float>(triangles[primitive].firstIndex);
	}
	return data;
}

RayQueryData RayQueryTriangles(std::span<const Triangle> triangles)
{
	RayQueryData data;
	data.triangles.reserve(triangles.size() * BVH_RAY_QUERY_TRIANGLE_FLOAT4_COUNT);
	auto add = [&data](const Triangle& triangle)
	{
		data.triangles.emplace_back(triangle.corners[0], std::bit_cast<float>(triangle.flags));
		data.triangles.emplace_back(triangle.corners[1], std::bit_cast<float>(triangle.firstIndex));
		data.triangles.emplace_back(triangle.corners[2], 0.0F);
	};
	for (const auto& triangle : triangles)
		if ((triangle.flags & BVH_TRIANGLE_FLAG_ALPHA_MASKED) == 0U)
			add(triangle);
	data.opaqueCount = static_cast<uint32_t>(data.triangles.size() / BVH_RAY_QUERY_TRIANGLE_FLOAT4_COUNT);
	for (const auto& triangle : triangles)
		if ((triangle.flags & BVH_TRIANGLE_FLAG_ALPHA_MASKED) != 0U)
			add(triangle);
	return data;
}

std::string Version()
{
	// bump the tag when Build changes what it makes
	return std::format(
		"tinybvh-{}.{}.{}-{}|bvh-v3{}",
		TINY_BVH_VERSION_MAJOR,
		TINY_BVH_VERSION_MINOR,
		TINY_BVH_VERSION_SUB,
		TINY_BVH_CACHE_VERSION,
		detail::SpatialSplits() ? "-sbvh" : "");
}

} // namespace gfx::bvh
