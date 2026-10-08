#pragma once

#include <gfx/gpu.h>
#include <gfx/shaders/capi.h>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace gfx
{

class Model;

// the main render target's color attachments (see FragmentOutput in the shaders), then its depth
constexpr uint32_t kColorAttachment = 0;
constexpr uint32_t kDepthAttachment = 1;
// opaque draws write the color; blended ones nothing (with FragmentTransparent, the layout's second fragment entry point,
// they go in the per pixel lists of the order independent transparency, see OitNode), so they needn't be sorted
constexpr std::array kOpaqueBlend{rhi::BlendMode::kOpaque, rhi::BlendMode::kNone, rhi::BlendMode::kNone, rhi::BlendMode::kNone};
constexpr std::array kTransparentBlend{rhi::BlendMode::kNone, rhi::BlendMode::kNone, rhi::BlendMode::kNone, rhi::BlendMode::kNone};
constexpr uint8_t kTransparentFragmentShader = 1;
// the shadow pass's fragments: depth only, cut out by alpha masks (FragmentShadow, the layout's third fragment entry
// point). with no color attachments, a kOpaque first blend mode is what makes the pipeline write depth
constexpr uint8_t kShadowFragmentShader = 2;
static_assert(kOpaqueBlend.size() == rhi::kMaxColorAttachments);

// the materials of the loaded model are 1 and up in gMaterialData (0 is the default material)
constexpr uint32_t kModelMaterialMaxCount = SHADER_TYPES_MATERIAL_COUNT - 1;

// the gMaterialData slot drawn for a submesh's material (-1: none)
[[nodiscard]] uint32_t ModelMaterialSlot(int32_t material);

// the main pass draws in two phases when the model has transmissive materials: the opaque ones that aren't, then (after
// the color is copied to the transmission texture) the transmissive and blended ones. else the first draws everything.
enum class MainPassPhase : uint8_t
{
	kOpaque,
	kTransmissive,
};

// one draw: a range of indices, of instances (gModelInstances from firstInstance, by SV_InstanceID), with the pipeline
// variant, dynamic state and per draw push constants it needs
struct DrawItem
{
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	uint32_t firstInstance = 0;
	uint32_t instanceCount = 0;
	rhi::GraphicsPipelineVariant variant{.blend = kOpaqueBlend};
	rhi::CullMode cullMode = rhi::CullMode::kBack;
	rhi::FrontFace frontFace = rhi::FrontFace::kCounterClockwise;
	uint32_t materialSlot = 0;
	uint32_t jointBase = SHADER_TYPES_NOT_SKINNED;
	uint32_t morphTargetCount = 0;
	uint32_t morphDeltaBase = 0;
	uint32_t morphFirstVertex = 0;
	uint32_t morphWeightBase = 0;
};

// what a view draws of the scene, in order, from one index buffer
struct DrawList
{
	const Buffer* indexBuffer = nullptr;
	std::vector<DrawItem> items;

	[[nodiscard]] bool Empty() const noexcept { return items.empty(); }
};

// a model's draws for a phase of the main pass: per submesh (material and topology, see InstallModel for where the
// materials are) its instances, the mirroring ones separately (they reverse the winding: clockwise front faces); the
// opaque submeshes first, then the blended ones (in any order, see kTransparentBlend), which depth test against them.
// with transmissive ones (two phases, see MainPassPhase), those and the blended ones in the second phase.
// transmissive(material) says whether a material is.
[[nodiscard]] DrawList BuildDrawList(
	const Model& model, MainPassPhase phase, bool twoPhases, const std::function<bool(size_t material)>& transmissive);

// a model's draws for the shadow pass: the triangles of the submeshes that cast shadows (opaque or alpha masked, not
// blended or transmissive)
[[nodiscard]] DrawList BuildShadowDrawList(const Model& model, const std::function<bool(size_t material)>& transmissive);

// records a draw list for a view (in its viewport, set by the caller): binds the pipeline variants it needs, from the
// default (opaque triangle list) one, which the caller bound and gets back
void RecordDrawList(
	CommandBufferHandle cmd, Pipeline& pipeline, const DrawList& list, PushConstants pushConstants, uint16_t viewIndex);

} // namespace gfx
