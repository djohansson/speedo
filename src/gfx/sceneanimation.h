#pragma once

#include <gfx/shaders/capi.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gfx
{

// what a model file animates (gltf nodes, skins and animations), kept for drawing it moving: the node hierarchy at
// rest, the skins whose joints are nodes, the animations that move the nodes, and which instances follow which node.
// everything that doesn't move is flattened at import as before.

// a column major 4x4 transform
using SceneMatrix = std::array<float, 16>;

// a node of the hierarchy: its parent (an index into SceneAnimationData::nodes, or -1 for a root) and its rest
// transform, translation * rotation * scale (or a matrix, which animations don't target)
struct SceneNode
{
	int32_t parent = -1;
	std::array<float, 3> translation{};
	std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F}; // a quaternion, xyzw
	std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
	bool hasMatrix = false;
	SceneMatrix matrix{};
};

// the joints a skinned mesh's vertices are weighted to: joint matrix j = world(joints[j]) * inverseBindMatrices[j],
// at jointBase + j in the joint matrix buffer (see SkinVertex in gfx/shaders/capi.h)
struct SceneSkin
{
	std::vector<uint32_t> joints; // node indices
	std::vector<SceneMatrix> inverseBindMatrices;
	uint32_t jointBase = 0;
};

// an animation channel: one property of one node over time, sampled at times (seconds) with values (3 floats per key
// for translation and scale, 4 for rotation; cubic splines have an in tangent, the value and an out tangent per key)
struct SceneAnimationChannel
{
	enum class Path : uint8_t
	{
		kTranslation,
		kRotation,
		kScale,
	};
	enum class Interpolation : uint8_t
	{
		kStep,
		kLinear,
		kCubicSpline,
	};

	uint32_t node = 0;
	Path path = Path::kTranslation;
	Interpolation interpolation = Interpolation::kLinear;
	std::vector<float> times;
	std::vector<float> values;
};

struct SceneAnimation
{
	std::string name;
	float duration = 0.0F; // the latest key time of its channels, in seconds
	std::vector<SceneAnimationChannel> channels;
};

// an instance whose transform follows a node: world(node) * local (local: the EXT_mesh_gpu_instancing instance's
// transform, else the identity). the other instances keep the transform they were imported with.
struct SceneInstanceLink
{
	uint32_t instance = 0;
	uint32_t node = 0;
	SceneMatrix local{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct SceneAnimationData
{
	std::vector<SceneNode> nodes; // all of the file's nodes, by their gltf index. empty if nothing moves or is skinned
	std::vector<SceneSkin> skins;
	std::vector<SceneAnimation> animations;
	std::vector<SceneInstanceLink> instanceLinks;
	uint32_t jointCount = 0; // of all skins

	[[nodiscard]] bool Empty() const noexcept { return skins.empty() && instanceLinks.empty(); }
};

// the nodes' world transforms with animation playing at time (seconds, wrapped to its duration), or at rest if
// animation is out of range
[[nodiscard]] std::vector<SceneMatrix> EvaluateNodes(const SceneAnimationData& data, size_t animation, float time);

// what moves, from the nodes' world transforms: the linked instances' transforms (written at their instance index,
// with their inverse transposes, as ModelInstance in gfx/shaders/capi.h) and the joint matrices (jointCount of them)
void WriteInstances(const SceneAnimationData& data, std::span<const SceneMatrix> worlds, std::span<std::byte> modelInstances);
void WriteJoints(const SceneAnimationData& data, std::span<const SceneMatrix> worlds, std::span<SceneMatrix> joints);

// the joint matrices at rest (jointCount of them), e.g. for the bounds of skinned vertices
[[nodiscard]] std::vector<SceneMatrix> RestJoints(const SceneAnimationData& data);

// a skinned vertex's position by its joints (as the vertex shader does): the sum of its weights times its joints'
// matrices (at jointBase + its joint indices in joints) times position
[[nodiscard]] std::array<float, 3> SkinPosition(
	std::span<const SceneMatrix> joints, uint32_t jointBase, const SkinVertex& skin, const std::array<float, 3>& position);

} // namespace gfx
