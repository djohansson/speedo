#include "drawlist.h"

#include <gfx/model.h>

#include <core/assert.h>
#include <core/profiling.h>
#include <core/taskexecutor.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

namespace gfx
{

using namespace rhi;

uint32_t ModelMaterialSlot(int32_t material)
{
	return material >= 0 ? static_cast<uint32_t>(material) + 1 : 0;
}

DrawList BuildDrawList(
	const Model& model,
	MainPassPhase phase,
	bool twoPhases,
	core::std_extra::function_ref<bool(size_t material)> transmissive,
	std::optional<core::std_extra::function_ref<uint16_t(size_t material)>> specialization)
{
	ZoneScopedN("gfx::BuildDrawList");

	const auto& desc = model.GetDesc();
	const auto& materials = desc.materials;
	const auto& skins = desc.animation.skins;

	DrawList list{.indexBuffer = &model.GetIndexBuffer()};
	auto add = [&](const ModelSubmesh& submesh, const std::array<BlendMode, kMaxColorAttachments>& blend, uint8_t fragmentShader)
	{
		// double sided materials' back faces are drawn too (the cull mode is dynamic state)
		bool doubleSided = submesh.material >= 0 && materials[submesh.material].doubleSided;
		DrawItem item{
			.firstIndex = submesh.firstIndex,
			.indexCount = submesh.indexCount,
			.variant =
				{.topology = submesh.topology,
				 .blend = blend,
				 .fragmentShader = fragmentShader,
				 .specialization = specialization ? (submesh.material >= 0 ? (*specialization)(static_cast<size_t>(submesh.material))
																		   : static_cast<uint16_t>(SHADER_TYPES_LAYERS_EXTENDED))
												  : uint16_t{0}},
			.cullMode = doubleSided ? CullMode::kNone : CullMode::kBack,
			.materialSlot = ModelMaterialSlot(submesh.material),
			.jointBase = submesh.skin >= 0 ? skins[submesh.skin].jointBase : SHADER_TYPES_NOT_SKINNED,
			.morphTargetCount = submesh.morphTargetCount,
			.morphDeltaBase = submesh.morphDeltaBase,
			.morphFirstVertex = submesh.morphFirstVertex,
			.morphWeightBase = submesh.morphWeightBase,
			.bounds = submesh.bounds,
			.cullable = !submesh.moves};

		auto unmirrored = submesh.instanceCount - submesh.mirroredInstanceCount;
		if (unmirrored > 0)
		{
			item.firstInstance = submesh.firstInstance;
			item.instanceCount = unmirrored;
			item.frontFace = FrontFace::kCounterClockwise;
			list.items.push_back(item);
		}
		if (submesh.mirroredInstanceCount > 0)
		{
			item.firstInstance = submesh.firstInstance + unmirrored;
			item.instanceCount = submesh.mirroredInstanceCount;
			item.frontFace = FrontFace::kClockwise;
			list.items.push_back(item);
		}
	};

	auto blended = [&materials](const ModelSubmesh& submesh) { return submesh.material >= 0 && materials[submesh.material].blend; };
	auto phaseOf = [&](const ModelSubmesh& submesh)
	{
		return twoPhases && submesh.material >= 0 && transmissive(static_cast<size_t>(submesh.material)) ? MainPassPhase::kTransmissive
																										  : MainPassPhase::kOpaque;
	};
	// each group by pipeline variant, for fewer pipeline switches: the opaque submeshes depth test, and the blended ones
	// are order independent (see FragmentTransparent), so their order doesn't matter
	auto byVariant = [&list](size_t first)
	{
		std::ranges::stable_sort(
			std::span(list.items).subspan(first),
			[](const DrawItem& a, const DrawItem& b)
			{
				return std::tie(a.variant.specialization, a.variant.topology, a.variant.fragmentShader) <
					   std::tie(b.variant.specialization, b.variant.topology, b.variant.fragmentShader);
			});
	};
	for (const auto& submesh : desc.submeshes)
		if (!blended(submesh) && phaseOf(submesh) == phase)
			add(submesh, kOpaqueBlend, 0);
	byVariant(0);
	auto opaqueCount = list.items.size();
	if (!twoPhases || phase == MainPassPhase::kTransmissive)
		for (const auto& submesh : desc.submeshes)
			if (blended(submesh))
				add(submesh, kTransparentBlend, kTransparentFragmentShader);
	byVariant(opaqueCount);

	return list;
}

DrawList BuildShadowDrawList(const Model& model, core::std_extra::function_ref<bool(size_t material)> transmissive)
{
	ZoneScopedN("gfx::BuildShadowDrawList");

	auto list = BuildDrawList(model, MainPassPhase::kOpaque, false, transmissive);
	const auto& materials = model.GetDesc().materials;
	std::erase_if(
		list.items,
		[&](const DrawItem& item)
		{
			auto material = static_cast<int32_t>(item.materialSlot) - 1;
			return item.variant.topology != PrimitiveTopology::kTriangleList || item.variant.fragmentShader != 0 ||
				   (material >= 0 && std::cmp_less(material, materials.size()) && transmissive(static_cast<size_t>(material)));
		});
	for (auto& item : list.items)
		item.variant.fragmentShader = kShadowFragmentShader;
	return list;
}

DrawList BuildScatterDrawList(
	const Model& model, core::std_extra::function_ref<bool(size_t material)> scatters, std::optional<core::std_extra::function_ref<uint16_t(size_t material)>> specialization)
{
	ZoneScopedN("gfx::BuildScatterDrawList");

	auto list = BuildDrawList(model, MainPassPhase::kOpaque, false, [](size_t) { return false; }, specialization);
	const auto& materials = model.GetDesc().materials;
	std::erase_if(
		list.items,
		[&](const DrawItem& item)
		{
			auto material = static_cast<int32_t>(item.materialSlot) - 1;
			return item.variant.topology != PrimitiveTopology::kTriangleList || item.variant.fragmentShader != 0 || material < 0 ||
				   std::cmp_greater_equal(material, materials.size()) || !scatters(static_cast<size_t>(material));
		});
	for (auto& item : list.items)
	{
		item.variant.fragmentShader = kScatterFragmentShader;
		item.variant.blend = kScatterBlend;
	}
	return list;
}

DrawList BuildScatterBackDrawList(const DrawList& scatter)
{
	DrawList list{.indexBuffer = scatter.indexBuffer};
	for (auto item : scatter.items)
	{
		if (item.cullMode != CullMode::kBack)
			continue;
		item.cullMode = CullMode::kFront;
		item.variant.fragmentShader = kShadowFragmentShader;
		list.items.push_back(item);
	}
	return list;
}

void WriteDrawRecords(std::span<DrawList* const> lists, std::span<DrawRecord> records)
{
	size_t next = 0;
	for (auto* list : lists)
	{
		if (list == nullptr)
			continue;
		for (auto& item : list->items)
		{
			ENSUREF(next < records.size(), "more than {} draws in a frame (SHADER_TYPES_DRAW_RECORD_COUNT)", records.size());
			item.record = static_cast<uint32_t>(next);
			records[next++] = DrawRecord{
					.materialId = item.materialSlot,
					.modelInstanceId = item.firstInstance,
					.jointBase = item.jointBase,
					.morphTargetCount = item.morphTargetCount,
					.morphDeltaBase = item.morphDeltaBase,
					.morphFirstVertex = item.morphFirstVertex,
				.morphWeightBase = item.morphWeightBase};
		}
	}
}

DrawStream::~DrawStream()
{
	End();
}

DrawStream::Command DrawStream::Take()
{
	ENSURE(myDevice != nullptr);
	if (myChunk < myChunks.size() && myUsedCount == kChunkCommandCount)
	{
		myChunk++;
		myUsedCount = 0;
	}
	if (myChunk == myChunks.size())
		myChunks.push_back({.buffer = std::make_unique<Buffer>(BufferCreateDesc{
								myDevice->CreateDeviceObjectCreateDesc(std::format("Draw Commands {}", myChunks.size())),
								kChunkCommandCount * sizeof(DrawIndexedIndirectCommand),
								BufferUsage::kIndirect,
								MemoryProperty::kHostVisible})});
	auto& chunk = myChunks[myChunk];
	if (chunk.memory.empty())
	{
		auto memory = chunk.buffer->Map();
		chunk.memory = std::span(reinterpret_cast<DrawIndexedIndirectCommand*>(memory.data()), kChunkCommandCount);
	}
	auto index = myUsedCount++;
	return {.buffer = chunk.buffer.get(), .index = index, .memory = &chunk.memory[index]};
}

void DrawStream::End()
{
	for (size_t chunkIt = 0; chunkIt < myChunks.size(); chunkIt++)
	{
		auto& chunk = myChunks[chunkIt];
		if (chunk.memory.empty())
			continue;
		auto used = chunkIt < myChunk ? kChunkCommandCount : myUsedCount;
		if (used > 0)
			chunk.buffer->Flush(0, used * sizeof(DrawIndexedIndirectCommand));
		chunk.buffer->Unmap();
		chunk.memory = {};
	}
	myChunk = 0;
	myUsedCount = 0;
}

void PlanDrawRuns(DrawList& list, uint32_t maxDrawCount)
{
	ENSURE(maxDrawCount > 0);
	list.runs.clear();
	for (uint32_t itemIt = 0; itemIt < list.items.size(); itemIt++)
	{
		const auto& item = list.items[itemIt];
		if (!list.runs.empty())
		{
			auto& run = list.runs.back();
			const auto& first = list.items[run.first];
			if (run.count < maxDrawCount && item.variant == first.variant && item.cullMode == first.cullMode &&
				item.frontFace == first.frontFace)
			{
				run.count++;
				continue;
			}
		}
		list.runs.push_back({.first = itemIt, .count = 1});
	}
}

namespace drawlist
{

// the pipeline and dynamic state a list's draws are recorded with, set only where it changes from the draw before
class DrawState final
{
public:
	DrawState(CommandBufferHandle cmd, Pipeline& pipeline, PushConstants pushConstants, core::TaskExecutor* executor)
		: myCmd(cmd)
		, myPipeline(pipeline)
		, myPushConstants(pushConstants)
		, myExecutor(executor)
	{}
	DrawState(const DrawState&) = delete;
	DrawState& operator=(const DrawState&) = delete;
	// gives the caller back the default (opaque triangle list) variant it bound
	~DrawState()
	{
		if (myBound != GraphicsPipelineVariant{.blend = kOpaqueBlend})
			myPipeline.BindPipelineAuto(myCmd, {.blend = kOpaqueBlend});
	}

	// whether drawing item needs other state than the draw before. a specialized variant (see
	// GraphicsPipelineVariant::specialization) is only drawn with once its pipeline exists: until then the generic one
	// draws, and the task creating it runs on the executor, the first time it is asked for
	[[nodiscard]] bool Changes(const DrawItem& item)
	{
		myVariant = item.variant;
		if (myVariant.specialization != 0 && myExecutor != nullptr)
		{
			// its creation takes tens of milliseconds
			auto request = myPipeline.RequestGraphicsPipeline(myVariant);
			if (request.handle == nullptr)
			{
				if (request.create)
					myExecutor->Submit({&request.create, 1});
				myVariant.specialization = 0;
			}
		}
		return myVariant != myBound || myCullMode != item.cullMode || myFrontFace != item.frontFace;
	}

	// sets the state Changes found item needs
	void Set(const DrawItem& item)
	{
		CommandEncoder encoder(myCmd);
		if (myVariant != myBound)
		{
			myBound = myVariant;
			myPipeline.BindPipelineAuto(myCmd, myVariant);
			// push constants outlive a pipeline bind with the same layout, as here, but variants switch rarely enough (the
			// draws are sorted by them) not to rely on it
			myPushed = false;
		}
		if (myCullMode != item.cullMode)
		{
			myCullMode = item.cullMode;
			encoder.SetCullMode(item.cullMode);
		}
		if (myFrontFace != item.frontFace)
		{
			myFrontFace = item.frontFace;
			encoder.SetFrontFace(item.frontFace);
		}
	}

	// before a draw: the push constants, if the pipeline bound since they were pushed
	void Push()
	{
		if (std::exchange(myPushed, true))
			return;
		myPipeline.PushConstants(myCmd, std::as_bytes(std::span(&myPushConstants, 1)));
	}

private:
	CommandBufferHandle myCmd;
	Pipeline& myPipeline;
	PushConstants myPushConstants;
	core::TaskExecutor* myExecutor = nullptr;
	GraphicsPipelineVariant myBound{.blend = kOpaqueBlend};
	GraphicsPipelineVariant myVariant{.blend = kOpaqueBlend}; // the one Changes found
	// the state the draw before set (none yet: a secondary command buffer starts without dynamic state)
	std::optional<CullMode> myCullMode;
	std::optional<FrontFace> myFrontFace;
	bool myPushed = false;
};

} // namespace drawlist

void RecordDrawList(
	CommandBufferHandle cmd,
	Pipeline& pipeline,
	const DrawList& list,
	PushConstants pushConstants,
	uint16_t viewIndex,
	DrawStream& stream,
	const Frustum* frustum,
	core::TaskExecutor* executor)
{
	ZoneScopedN("gfx::RecordDrawList");

	CommandEncoder encoder(cmd);
	pushConstants.viewId = viewIndex;
	drawlist::DrawState state(cmd, pipeline, pushConstants, executor);
	// indirect: the run of draws since the state last changed, one indirect draw of their consecutive commands
	const Buffer* runBuffer = nullptr;
	uint32_t runFirst = 0;
	uint32_t runCount = 0;
	auto flush = [&]
	{
		if (runCount == 0)
			return;
		state.Push();
		encoder.DrawIndexedIndirect(*runBuffer, runFirst * sizeof(DrawIndexedIndirectCommand), runCount, sizeof(DrawIndexedIndirectCommand));
		runCount = 0;
	};
	for (const auto& item : list.items)
	{
		if (frustum != nullptr && item.cullable && frustum->Excludes(item.bounds))
			continue;

		if (state.Changes(item))
		{
			flush();
			state.Set(item);
		}

		// the first instance names the draw's record (SV_StartInstanceLocation), which the shader adds the instance to
		if (stream.indirect)
		{
			auto command = stream.Take();
			if (runCount > 0 &&
				(command.buffer != runBuffer || runFirst + runCount != command.index || runCount == stream.GetMaxDrawCount()))
				flush();
			if (runCount == 0)
			{
				runBuffer = command.buffer;
				runFirst = command.index;
			}
			runCount++;
			*command.memory = DrawIndexedIndirectCommand{
				.indexCount = item.indexCount, .instanceCount = item.instanceCount, .firstIndex = item.firstIndex, .firstInstance = item.record};
		}
		else
		{
			state.Push();
			encoder.DrawIndexed(item.indexCount, item.instanceCount, item.firstIndex, 0, item.record);
		}
	}
	flush();
}

void RecordCulledDrawList(
	CommandBufferHandle cmd,
	Pipeline& pipeline,
	const DrawList& list,
	PushConstants pushConstants,
	uint16_t viewIndex,
	const CulledDraws& culled,
	core::TaskExecutor* executor)
{
	ZoneScopedN("gfx::RecordCulledDrawList");

	ENSURE(culled.commands != nullptr && culled.counts != nullptr);

	CommandEncoder encoder(cmd);
	pushConstants.viewId = viewIndex;
	drawlist::DrawState state(cmd, pipeline, pushConstants, executor);
	for (uint32_t runIt = 0; runIt < list.runs.size(); runIt++)
	{
		const auto& run = list.runs[runIt];
		const auto& item = list.items[run.first];
		if (state.Changes(item))
			state.Set(item);
		state.Push();
		encoder.DrawIndexedIndirectCount(
			*culled.commands,
			static_cast<uint64_t>(culled.commandBase + run.first) * sizeof(DrawIndexedIndirectCommand),
			*culled.counts,
			static_cast<uint64_t>(culled.countBase + runIt) * sizeof(uint32_t),
			run.count,
			sizeof(DrawIndexedIndirectCommand));
	}
}

} // namespace gfx
