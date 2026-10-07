#pragma once

#include <gfx/scenelight.h>
#include <gfx/shaders/capi.h>

#include <array>
#include <cstdint>
#include <optional>
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
		kWeights, // a node's morph target weights (see SceneMorph): its weightCount floats per key
		// KHR_animation_pointer's other targets: node is the index of a ScenePointerTarget, of valueCount floats per key
		kPointer,
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

// a node whose mesh's morph targets an animation moves (by its weights): its weights are weightCount floats from
// weightBase in the morph weight buffer (gMorphWeights, see MorphDelta in gfx/shaders/capi.h), which its submeshes read
struct SceneMorph
{
	uint32_t node = 0;
	uint32_t weightBase = 0;
	uint32_t weightCount = 0;
};

// a material's values a KHR_animation_pointer channel may set (see ScenePointerTarget::Kind::kMaterial), and their
// float counts (see PropertyComponents)
enum class MaterialProperty : uint16_t
{
	kBaseColor, // 4: the base color factor (whose material keeps it, rather than the vertex colors)
	kEmissive, // 3: the emissive factor (times its strength, ScenePointerTarget::scale)
	kMetallic,
	kRoughness,
	kAlphaCutoff,
	kNormalScale,
	kOcclusionStrength,
	kSpecular,
	kSpecularColor, // 3
	kIor,
	kClearcoat,
	kClearcoatRoughness,
	kClearcoatNormalScale,
	kSheenColor, // 3
	kSheenRoughness,
	kTransmission,
	kThickness,
	kAttenuationColor, // 3
	kAttenuationDistance,
	kDispersion,
	kAnisotropy,
	kIridescence,
	kIridescenceIor,
	kIridescenceThicknessMin,
	kIridescenceThicknessMax,
	kDiffuseTransmission,
	kDiffuseTransmissionColor, // 3
};

[[nodiscard]] constexpr uint16_t PropertyComponents(MaterialProperty property) noexcept
{
	switch (property)
	{
	case MaterialProperty::kBaseColor: return 4;
	case MaterialProperty::kEmissive:
	case MaterialProperty::kSpecularColor:
	case MaterialProperty::kSheenColor:
	case MaterialProperty::kAttenuationColor:
	case MaterialProperty::kDiffuseTransmissionColor: return 3;
	default: return 1;
	}
}

// a material's textures, whose KHR_texture_transform a KHR_animation_pointer channel may set
enum class MaterialTexture : uint16_t
{
	kBaseColor,
	kMetallicRoughness,
	kNormal,
	kOcclusion,
	kEmissive,
	kSpecular,
	kSpecularColor,
	kClearcoat,
	kClearcoatRoughness,
	kClearcoatNormal,
	kSheenColor,
	kSheenRoughness,
	kTransmission,
	kThickness,
	kAnisotropy,
	kIridescence,
	kIridescenceThickness,
	kDiffuseTransmission,
	kDiffuseTransmissionColor,
};

// what a KHR_animation_pointer channel (Path::kPointer) sets beyond a node's transform and weights: valueCount floats
// from valueBase in the pointer values (see EvaluatePointers), at rest SceneAnimationData::pointerDefaults'
struct ScenePointerTarget
{
	enum class Kind : uint8_t
	{
		kMaterial, // a material's value (index: the material, property: a MaterialProperty)
		// a material texture's KHR_texture_transform (index: the material, property: a MaterialTexture): its offset (2
		// floats), rotation (1, radians) and scale (2), always all three, in this order (see TextureRef::animatedTransform)
		kTextureOffset,
		kTextureRotation,
		kTextureScale,
		kNodeVisibility, // KHR_node_visibility's visible (index: the node): below 0.5 hides it and its descendants
	};

	Kind kind = Kind::kMaterial;
	uint32_t index = 0;
	uint16_t property = 0;
	uint16_t valueCount = 0;
	uint32_t valueBase = 0;
	float scale = 1.0F; // for kMaterial: what the values are multiplied by (the emissive strength, for kEmissive)
};

// a light that follows a node (its position, and its direction its -z), and its visibility: the lights at index
// light of the model's lights (see ModelDesc::lights, at rest)
struct SceneLightLink
{
	uint32_t light = 0;
	uint32_t node = 0;
};

struct SceneAnimationData
{
	std::vector<SceneNode> nodes; // all of the file's nodes, by their gltf index. empty if nothing moves or is skinned
	std::vector<SceneSkin> skins;
	std::vector<SceneAnimation> animations;
	std::vector<SceneInstanceLink> instanceLinks;
	std::vector<SceneMorph> morphs;
	std::vector<ScenePointerTarget> pointerTargets;
	std::vector<float> pointerDefaults; // the pointer values at rest
	std::vector<SceneLightLink> lightLinks;
	uint32_t jointCount = 0; // of all skins

	[[nodiscard]] bool Empty() const noexcept
	{
		return skins.empty() && instanceLinks.empty() && morphs.empty() && pointerTargets.empty() && lightLinks.empty();
	}
};

// the nodes' world transforms with animation playing at time (seconds, wrapped to its duration), or at rest if
// animation is out of range
[[nodiscard]] std::vector<SceneMatrix> EvaluateNodes(const SceneAnimationData& data, size_t animation, float time);

// an animation (an index into SceneAnimationData::animations, or nullopt for the rest pose) at a time, in seconds
struct ScenePose
{
	std::optional<size_t> animation;
	float time = 0.0F;
};

// the nodes' world transforms with pose crossfaded in over from: their local transforms blended by weight (0: from's,
// 1: pose's; translation and scale lerped, rotation slerped), as when one animation fades into another
[[nodiscard]] std::vector<SceneMatrix> EvaluateNodes(const SceneAnimationData& data, const ScenePose& pose, const ScenePose& from, float weight);

// the morph target weights (see SceneMorph) with pose crossfaded in over from (lerped by weight): defaults (a node's, else
// its mesh's, see ModelDesc::morphWeights) where the animations don't move them
[[nodiscard]] std::vector<float> EvaluateWeights(
	const SceneAnimationData& data, std::span<const float> defaults, const ScenePose& pose, const ScenePose& from, float weight);

// the pointer values (see ScenePointerTarget) with pose crossfaded in over from (lerped by weight)
[[nodiscard]] std::vector<float> EvaluatePointers(const SceneAnimationData& data, const ScenePose& pose, const ScenePose& from, float weight);

// whether each node is visible by the pointer values (KHR_node_visibility, inherited by the descendants): all are with
// none
[[nodiscard]] std::vector<uint8_t> NodeVisibility(const SceneAnimationData& data, std::span<const float> pointerValues);

// what moves, from the nodes' world transforms: the linked instances' transforms (written at their instance index,
// with their inverse transposes, as ModelInstance in gfx/shaders/capi.h; collapsed to a point where their node isn't
// visible, if visible isn't empty) and the joint matrices (jointCount of them)
void WriteInstances(
	const SceneAnimationData& data, std::span<const SceneMatrix> worlds, std::span<const uint8_t> visible, std::span<std::byte> modelInstances);
// the linked lights, from their rest values (restLights) where their nodes put them, dark where they aren't visible
void WriteLights(
	const SceneAnimationData& data, std::span<const SceneMatrix> worlds, std::span<const uint8_t> visible,
	std::span<const SceneLight> restLights, std::span<SceneLight> lights);
void WriteJoints(const SceneAnimationData& data, std::span<const SceneMatrix> worlds, std::span<SceneMatrix> joints);

// the joint matrices at rest (jointCount of them), e.g. for the bounds of skinned vertices
[[nodiscard]] std::vector<SceneMatrix> RestJoints(const SceneAnimationData& data);

// a skinned vertex's position by its joints (as the vertex shader does): the sum of its weights times its joints'
// matrices (at jointBase + its joint indices in joints) times position
[[nodiscard]] std::array<float, 3> SkinPosition(
	std::span<const SceneMatrix> joints, uint32_t jointBase, const SkinVertex& skin, const std::array<float, 3>& position);

} // namespace gfx
