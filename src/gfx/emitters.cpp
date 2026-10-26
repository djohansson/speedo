#include "emitters.h"

#include <gfx/model.h>
#include <gfx/sceneanimation.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace gfx::emitters
{

namespace detail
{

static float Luminance(const glm::vec3& color)
{
	return (0.2126F * color.x) + (0.7152F * color.y) + (0.0722F * color.z);
}

// whether an animation sets a material's emission (KHR_animation_pointer's emissive factor or strength): it then emits
// while its factor at rest may be black
static bool EmissionAnimates(const ModelDesc& desc, size_t material)
{
	for (const auto& target : desc.animation.pointerTargets)
		if (target.kind == ScenePointerTarget::Kind::kMaterial && target.index == material &&
			(MaterialProperty{target.property} == MaterialProperty::kEmissive ||
			 MaterialProperty{target.property} == MaterialProperty::kEmissiveStrength))
			return true;
	return false;
}

// what a node of the tree holds (see EmitterNode): a box, a cone of the directions it faces (cosine 1: one direction,
// -1: all of them), whether any of it emits both ways, and its power. empty: no power
struct Bounds
{
	glm::vec3 low{std::numeric_limits<float>::max()};
	glm::vec3 high{std::numeric_limits<float>::lowest()};
	glm::vec3 axis{0.0F, 0.0F, 1.0F};
	float cosine = 1.0F;
	bool doubleSided = false;
	double power = 0.0;
};

// the smallest cone holding two (pbrt's DirectionCone::Union)
static void UniteCones(const glm::vec3& axisA, float cosineA, const glm::vec3& axisB, float cosineB, glm::vec3& axis, float& cosine)
{
	constexpr float kPi = std::numbers::pi_v<float>;
	float angleA = std::acos(std::clamp(cosineA, -1.0F, 1.0F));
	float angleB = std::acos(std::clamp(cosineB, -1.0F, 1.0F));
	float between = std::acos(std::clamp(glm::dot(axisA, axisB), -1.0F, 1.0F));
	if (std::min(between + angleB, kPi) <= angleA)
	{
		axis = axisA;
		cosine = cosineA;
		return;
	}
	if (std::min(between + angleA, kPi) <= angleB)
	{
		axis = axisB;
		cosine = cosineB;
		return;
	}
	float angle = (angleA + between + angleB) / 2.0F;
	glm::vec3 across = glm::cross(axisA, axisB);
	if (angle >= kPi || glm::dot(across, across) == 0.0F)
	{
		axis = axisA;
		cosine = -1.0F;
		return;
	}
	axis = glm::normalize(glm::angleAxis(angle - angleA, glm::normalize(across)) * axisA);
	cosine = std::cos(angle);
}

static Bounds Unite(const Bounds& a, const Bounds& b)
{
	if (!(a.power > 0.0))
		return b;
	if (!(b.power > 0.0))
		return a;
	Bounds united;
	united.low = glm::min(a.low, b.low);
	united.high = glm::max(a.high, b.high);
	UniteCones(a.axis, a.cosine, b.axis, b.cosine, united.axis, united.cosine);
	united.doubleSided = a.doubleSided || b.doubleSided;
	united.power = a.power + b.power;
	return united;
}

// what a split costs (pbrt's BVHLightSampler::EvaluateCost, its surface area orientation heuristic): the power, times the
// solid angle it may light (its cone widened by the emission's quarter turn), times its box's area, times how much
// narrower the box is along the axis split than along its longest (keeps the boxes from getting thin)
static double Cost(const Bounds& bounds, const glm::vec3& extent, int axis)
{
	constexpr double kPi = std::numbers::pi;
	if (!(bounds.power > 0.0))
		return 0.0;
	double cosine = bounds.doubleSided ? -1.0 : bounds.cosine;
	double angle = std::acos(std::clamp(cosine, -1.0, 1.0));
	double widened = std::min(angle + (kPi / 2.0), kPi);
	double sine = std::sqrt(std::max(1.0 - (cosine * cosine), 0.0));
	double solidAngle = (2.0 * kPi * (1.0 - cosine)) +
						(kPi / 2.0 * ((2.0 * widened * sine) - std::cos(angle - (2.0 * widened)) - (2.0 * angle * sine) + cosine));
	glm::dvec3 size = glm::max(glm::dvec3(bounds.high - bounds.low), glm::dvec3(0.0));
	double area = 2.0 * ((size.x * size.y) + (size.y * size.z) + (size.z * size.x));
	double longest = std::max({extent.x, extent.y, extent.z});
	double thinness = extent[axis] > 0.0F ? longest / extent[axis] : 1.0;
	return bounds.power * solidAngle * std::max(area, 1e-30) * thinness;
}

// the tree over leaves[first, last) (each a triangle's bounds and index), its nodes appended depth first: split by the
// cost of 12 buckets along each axis, at the median by count from depth 32 on (a run of uneven splits would otherwise go
// past EMITTER_NODE_DEPTH_MOST) or where every centroid is the same
static Bounds BuildTree(
	std::vector<std::pair<Bounds, uint32_t>>& leaves, size_t first, size_t last, uint32_t depth, std::vector<EmitterNode>& nodes)
{
	ENSUREF(depth < EMITTER_NODE_DEPTH_MOST, "the emitters' tree goes deeper than {}", EMITTER_NODE_DEPTH_MOST);
	ENSUREF(nodes.size() <= EMITTER_NODE_INDEX_MASK, "the emitters' tree holds at most {} nodes", EMITTER_NODE_INDEX_MASK);
	size_t node = nodes.size();
	nodes.emplace_back();
	auto write = [&nodes, node](const Bounds& bounds, uint32_t bits)
	{
		auto& written = nodes[node];
		for (int axisIt = 0; axisIt < 3; axisIt++)
		{
			written.low[axisIt] = bounds.low[axisIt];
			written.high[axisIt] = bounds.high[axisIt];
			written.axis[axisIt] = bounds.axis[axisIt];
		}
		written.low[3] = static_cast<float>(bounds.power);
		written.high[3] = bounds.cosine;
		written.axis[3] = std::bit_cast<float>(bits | (bounds.doubleSided ? EMITTER_NODE_DOUBLE_SIDED : 0U));
	};
	if (last - first == 1)
	{
		write(leaves[first].first, EMITTER_NODE_LEAF | leaves[first].second);
		return leaves[first].first;
	}

	glm::vec3 low(std::numeric_limits<float>::max());
	glm::vec3 high(std::numeric_limits<float>::lowest());
	Bounds all;
	for (size_t leafIt = first; leafIt < last; leafIt++)
	{
		glm::vec3 centroid = (leaves[leafIt].first.low + leaves[leafIt].first.high) * 0.5F;
		low = glm::min(low, centroid);
		high = glm::max(high, centroid);
		all = Unite(all, leaves[leafIt].first);
	}
	glm::vec3 extent = all.high - all.low;
	glm::vec3 centroidExtent = high - low;
	size_t middle = first + ((last - first) / 2);
	bool split = false;
	if (depth < 32 && glm::any(glm::greaterThan(centroidExtent, glm::vec3(0.0F))))
	{
		constexpr int kBucketCount = 12;
		double best = std::numeric_limits<double>::max();
		int bestAxis = -1;
		int bestBucket = -1;
		auto bucketOf = [&](const Bounds& bounds, int axis)
		{
			float centroid = (bounds.low[axis] + bounds.high[axis]) * 0.5F;
			int bucket = static_cast<int>(static_cast<float>(kBucketCount) * (centroid - low[axis]) / centroidExtent[axis]);
			return std::clamp(bucket, 0, kBucketCount - 1);
		};
		for (int axis = 0; axis < 3; axis++)
		{
			if (!(centroidExtent[axis] > 0.0F))
				continue;
			std::array<Bounds, kBucketCount> buckets{};
			for (size_t leafIt = first; leafIt < last; leafIt++)
			{
				auto& bucket = buckets[static_cast<size_t>(bucketOf(leaves[leafIt].first, axis))];
				bucket = Unite(bucket, leaves[leafIt].first);
			}
			// what each split leaves below and above it
			std::array<Bounds, kBucketCount> below{};
			std::array<Bounds, kBucketCount> above{};
			below[0] = buckets[0];
			above[kBucketCount - 1] = buckets[kBucketCount - 1];
			for (size_t bucketIt = 1; bucketIt < kBucketCount; bucketIt++)
			{
				below[bucketIt] = Unite(below[bucketIt - 1], buckets[bucketIt]);
				above[kBucketCount - 1 - bucketIt] = Unite(above[kBucketCount - bucketIt], buckets[kBucketCount - 1 - bucketIt]);
			}
			for (int splitIt = 0; splitIt < kBucketCount - 1; splitIt++)
			{
				const auto& under = below[static_cast<size_t>(splitIt)];
				const auto& over = above[static_cast<size_t>(splitIt) + 1];
				if (!(under.power > 0.0) || !(over.power > 0.0))
					continue;
				double cost = Cost(under, extent, axis) + Cost(over, extent, axis);
				if (cost < best)
				{
					best = cost;
					bestAxis = axis;
					bestBucket = splitIt;
				}
			}
		}
		if (bestAxis >= 0)
		{
			auto* partition = std::partition(
				leaves.data() + first, leaves.data() + last, [&](const auto& leaf) { return bucketOf(leaf.first, bestAxis) <= bestBucket; });
			middle = static_cast<size_t>(partition - leaves.data());
			split = middle > first && middle < last;
		}
	}
	if (!split)
	{
		middle = first + ((last - first) / 2);
		int axis = centroidExtent.x > centroidExtent.y ? (centroidExtent.x > centroidExtent.z ? 0 : 2) : (centroidExtent.y > centroidExtent.z ? 1 : 2);
		std::nth_element(
			leaves.begin() + static_cast<ptrdiff_t>(first),
			leaves.begin() + static_cast<ptrdiff_t>(middle),
			leaves.begin() + static_cast<ptrdiff_t>(last),
			[axis](const auto& a, const auto& b) { return a.first.low[axis] + a.first.high[axis] < b.first.low[axis] + b.first.high[axis]; });
	}
	Bounds below = BuildTree(leaves, first, middle, depth + 1, nodes);
	auto second = static_cast<uint32_t>(nodes.size());
	Bounds above = BuildTree(leaves, middle, last, depth + 1, nodes);
	Bounds bounds = Unite(below, above);
	write(bounds, second);
	return bounds;
}

} // namespace detail

Data Build(const ModelDesc& desc, std::span<const uint32_t> indices, std::span<const VertexP3fN3fTa4fT014fC4f> vertices)
{
	ZoneScopedN("gfx::emitters::Build");

	Data data;
	std::vector<std::pair<detail::Bounds, uint32_t>> leaves;
	for (const auto& submesh : desc.submeshes)
	{
		if (submesh.moves || submesh.topology != rhi::PrimitiveTopology::kTriangleList || submesh.material < 0)
			continue;
		const auto& material = desc.materials[static_cast<size_t>(submesh.material)];
		float luminance = detail::Luminance(material.emissive);
		// an animated emission is weighed as if it were 1 where it is black at rest (the shaders weigh each point by its
		// emission as the frame has it: a pick's probability needn't be right, only not 0 where it emits)
		if (detail::EmissionAnimates(desc, static_cast<size_t>(submesh.material)))
			luminance = std::max(luminance, 1.0F);
		if (!(luminance > 0.0F))
			continue;
		ENSUREF(
			static_cast<uint64_t>(submesh.material + 1) < (1ULL << 31U),
			"an emitter's material index must fit in 31 bits, not {}", submesh.material + 1);
		uint32_t flags = material.doubleSided ? EMITTER_FLAG_DOUBLE_SIDED : 0U;
		for (uint32_t instanceIt = 0; instanceIt < submesh.instanceCount; instanceIt++)
		{
			const glm::mat4& transform = desc.instances[submesh.firstInstance + instanceIt];
			// a mirroring instance reverses the winding: its front is the other way round
			bool mirrored = glm::determinant(glm::mat3(transform)) < 0.0F;
			for (uint32_t indexIt = 0; indexIt + 2 < submesh.indexCount; indexIt += 3)
			{
				std::array<glm::vec3, 3> corners{};
				for (uint32_t cornerIt = 0; cornerIt < 3; cornerIt++)
				{
					const auto& position = vertices[indices[submesh.firstIndex + indexIt + cornerIt]].position;
					corners[cornerIt] = glm::vec3(transform * glm::vec4(position[0], position[1], position[2], 1.0F));
				}
				glm::vec3 edge1 = corners[1] - corners[0];
				glm::vec3 edge2 = corners[2] - corners[0];
				if (mirrored)
					std::swap(edge1, edge2);
				glm::vec3 across = glm::cross(edge1, edge2);
				double area = 0.5 * static_cast<double>(glm::length(across));
				if (!(area > 0.0))
					continue;
				ENSUREF(data.triangles.size() < EMITTER_NODE_INDEX_MASK, "at most {} emitter triangles", EMITTER_NODE_INDEX_MASK);
				auto index = static_cast<uint32_t>(data.triangles.size());
				// the texture's coordinates are read by the triangle's first index: a mirrored one's are swapped too
				EmitterTriangle& triangle = data.triangles.emplace_back();
				triangle.corner[0] = corners[0].x;
				triangle.corner[1] = corners[0].y;
				triangle.corner[2] = corners[0].z;
				triangle.corner[3] = std::bit_cast<float>(static_cast<uint32_t>(submesh.material + 1));
				triangle.edge1[0] = edge1.x;
				triangle.edge1[1] = edge1.y;
				triangle.edge1[2] = edge1.z;
				triangle.edge1[3] = std::bit_cast<float>(submesh.firstIndex + indexIt);
				triangle.edge2[0] = edge2.x;
				triangle.edge2[1] = edge2.y;
				triangle.edge2[2] = edge2.z;
				triangle.edge2[3] = std::bit_cast<float>(flags | (mirrored ? EMITTER_FLAG_MIRRORED : 0U));
				detail::Bounds bounds;
				bounds.low = glm::min(corners[0], glm::min(corners[1], corners[2]));
				bounds.high = glm::max(corners[0], glm::max(corners[1], corners[2]));
				bounds.axis = glm::normalize(across);
				bounds.doubleSided = material.doubleSided;
				bounds.power = area * static_cast<double>(luminance);
				leaves.emplace_back(bounds, index);
				data.power += bounds.power;
				data.area += area;
			}
		}
	}
	if (!leaves.empty())
	{
		data.nodes.reserve((2 * leaves.size()) - 1);
		detail::BuildTree(leaves, 0, leaves.size(), 0, data.nodes);
	}
	return data;
}

} // namespace gfx::emitters
