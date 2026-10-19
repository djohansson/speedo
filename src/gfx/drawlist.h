#pragma once

#include <gfx/bounds.h>
#include <gfx/frustum.h>
#include <gfx/gpu.h>
#include <gfx/shaders/capi.h>

#include <core/std_extra.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace core
{
class TaskExecutor;
} // namespace core

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
// the scatter pass's fragments: the light entering scattering materials (FragmentScatter, the fourth), and their
// multiscatter color, in its two color attachments
constexpr uint8_t kScatterFragmentShader = 3;
constexpr std::array kScatterBlend{rhi::BlendMode::kOpaque, rhi::BlendMode::kOpaque, rhi::BlendMode::kNone, rhi::BlendMode::kNone};
static_assert(kOpaqueBlend.size() == rhi::kMaxColorAttachments);

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
	// where it is drawn (its chunk's bounds, see mesh::Chunk), and whether a view may skip it by them: not if it moves
	Bounds3f bounds;
	bool cullable = false;
	// its DrawRecord in the frame's gDrawRecords (see WriteDrawRecords)
	uint32_t record = 0;
};

// a run of a draw list's items with the same pipeline variant and dynamic state (see PlanDrawRuns)
struct DrawRun
{
	uint32_t first = 0;
	uint32_t count = 0;
};

// what a view draws of the scene, in order, from one index buffer
struct DrawList
{
	const Buffer* indexBuffer = nullptr;
	std::vector<DrawItem> items;
	std::vector<DrawRun> runs; // see PlanDrawRuns

	[[nodiscard]] bool Empty() const noexcept { return items.empty(); }
};

// a model's draws for a phase of the main pass: per submesh (material and topology, see InstallModel for where the
// materials are) its instances, the mirroring ones separately (they reverse the winding: clockwise front faces); the
// opaque submeshes first, then the blended ones (in any order, see kTransparentBlend), which depth test against them.
// with transmissive ones (two phases, see MainPassPhase), those and the blended ones in the second phase.
// transmissive(material) says whether a material is, and specialization(material) the layer groups its pipeline can
// skip (SHADER_TYPES_LAYERS_*, see GraphicsPipelineVariant::specialization: none for the generic pipelines)
[[nodiscard]] DrawList BuildDrawList(
	const Model& model,
	MainPassPhase phase,
	bool twoPhases,
	core::std_extra::function_ref<bool(size_t material)> transmissive,
	std::optional<core::std_extra::function_ref<uint16_t(size_t material)>> specialization = std::nullopt);

// a model's draws for the shadow pass: the triangles of the submeshes that cast shadows (opaque or alpha masked, not
// blended or transmissive)
[[nodiscard]] DrawList BuildShadowDrawList(const Model& model, core::std_extra::function_ref<bool(size_t material)> transmissive);

// a model's draws for the scatter pass: the triangles of the submeshes whose material scatters (KHR_materials_volume_scatter),
// unless blended
[[nodiscard]] DrawList BuildScatterDrawList(
	const Model& model,
	core::std_extra::function_ref<bool(size_t material)> scatters,
	std::optional<core::std_extra::function_ref<uint16_t(size_t material)>> specialization = std::nullopt);

// the scatter pass's draws, for their back faces' depth (FragmentShadow, the front faces culled): single sided ones only,
// since a double sided surface's nearest back face is its front
[[nodiscard]] DrawList BuildScatterBackDrawList(const DrawList& scatter);

// cuts a list's items into runs of the same pipeline variant, cull mode and front face, of at most maxDrawCount
// (DeviceLimits::maxDrawIndirectCount): each an indirect draw when the gpu culls (see RecordCulledDrawList)
void PlanDrawRuns(DrawList& list, uint32_t maxDrawCount);

// gives each draw of lists its DrawRecord (materialSlot and on), in order, written to records (DrawItem::record), which
// must hold them all
void WriteDrawRecords(std::span<DrawList* const> lists, std::span<DrawRecord> records);

// a frame index's indirect draws' commands, handed out in recording order (Take, by RecordDrawList) from chunks of
// kChunkCommandCount, made as the frame needs them and kept for its next use. the draw thread's: a chunk is mapped from the
// first command taken from it until End, which the frame calls once it has recorded
class DrawStream final
{
public:
	static constexpr uint32_t kChunkCommandCount = 1U << 14;

	DrawStream() = default;
	explicit DrawStream(Device& device) : myDevice(&device), myMaxDrawCount(device.GetLimits().maxDrawIndirectCount) {}
	DrawStream(const DrawStream&) = delete;
	DrawStream(DrawStream&&) noexcept = default;
	~DrawStream();
	DrawStream& operator=(const DrawStream&) = delete;
	DrawStream& operator=(DrawStream&&) noexcept = default;

	// a command's place: its buffer (BufferUsage::kIndirect), its index in it, and its memory, to write it to
	struct Command
	{
		const Buffer* buffer = nullptr;
		uint32_t index = 0;
		DrawIndexedIndirectCommand* memory = nullptr;
	};
	[[nodiscard]] Command Take();
	// makes the commands taken visible to the gpu and unmaps their chunks, for the next frame to start over
	void End();

	// each run of draws with the same pipeline and state as one indirect draw, else direct draws (see RecordDrawList)
	bool indirect = true;

	// the most draws one indirect draw may make (DeviceLimits::maxDrawIndirectCount)
	[[nodiscard]] uint32_t GetMaxDrawCount() const noexcept { return myMaxDrawCount; }

private:
	struct Chunk
	{
		std::unique_ptr<Buffer> buffer;
		std::span<DrawIndexedIndirectCommand> memory; // while mapped
	};

	Device* myDevice = nullptr;
	uint32_t myMaxDrawCount = 1;
	std::vector<Chunk> myChunks;
	size_t myChunk = 0; // the one commands are taken from
	uint32_t myUsedCount = 0; // of its commands
};

// records a draw list for a view (in its viewport, set by the caller): the draws not outside frustum (if given), with
// their state set only where it changes from the draw before (chunks of a submesh share it), each draw naming its
// DrawRecord by its first instance (an indirect draw's commands from stream). binds
// the pipeline variants it needs, from the default (opaque triangle list) one, which the caller bound and gets back. a specialized variant (see
// GraphicsPipelineVariant::specialization) is only drawn with once its pipeline exists: until then the generic one
// draws, and the task creating it (see Pipeline::RequestGraphicsPipeline) runs on executor, the first time it is asked for
void RecordDrawList(
	CommandBufferHandle cmd,
	Pipeline& pipeline,
	const DrawList& list,
	PushConstants pushConstants,
	uint16_t viewIndex,
	DrawStream& stream,
	const Frustum* frustum = nullptr,
	core::TaskExecutor* executor = nullptr);

// where the gpu's culling (the Cull pass, ComputeCull) put a list's draws for a view: a place per item from commandBase
// in commands, each run's from its first item's, and a count per run from countBase in counts
struct CulledDraws
{
	const Buffer* commands = nullptr;
	const Buffer* counts = nullptr;
	uint32_t commandBase = 0;
	uint32_t countBase = 0;
};

// records a draw list for a view as the gpu culled it: each of its runs (see PlanDrawRuns) one indirect draw of as many
// of its commands as its count says. binds the pipeline variants it needs as RecordDrawList does
void RecordCulledDrawList(
	CommandBufferHandle cmd,
	Pipeline& pipeline,
	const DrawList& list,
	PushConstants pushConstants,
	uint16_t viewIndex,
	const CulledDraws& culled,
	core::TaskExecutor* executor = nullptr);

} // namespace gfx
