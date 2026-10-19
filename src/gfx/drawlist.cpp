#include "drawlist.h"

#include <gfx/model.h>

#include <core/profiling.h>
#include <core/taskexecutor.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <print>
#include <span>
#include <tuple>
#include <utility>

namespace gfx
{

using namespace rhi;

uint32_t ModelMaterialSlot(int32_t material)
{
	return material >= 0 && std::cmp_less(material, kModelMaterialMaxCount) ? static_cast<uint32_t>(material) + 1 : 0;
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
	bool dropped = false;
	for (auto* list : lists)
	{
		if (list == nullptr)
			continue;
		std::erase_if(
			list->items,
			[&](DrawItem& item)
			{
				if (next == records.size())
				{
					dropped = true;
					return true;
				}
				item.record = static_cast<uint32_t>(next);
				records[next++] = DrawRecord{
					.materialId = item.materialSlot,
					.modelInstanceId = item.firstInstance,
					.jointBase = item.jointBase,
					.morphTargetCount = item.morphTargetCount,
					.morphDeltaBase = item.morphDeltaBase,
					.morphFirstVertex = item.morphFirstVertex,
					.morphWeightBase = item.morphWeightBase};
				return false;
			});
	}
	static std::atomic_bool gWarned = false;
	if (dropped && !gWarned.exchange(true))
		std::println(stderr, "more than {} draws in a frame: the rest aren't drawn", records.size());
}

void RecordDrawList(
	CommandBufferHandle cmd,
	Pipeline& pipeline,
	const DrawList& list,
	PushConstants pushConstants,
	uint16_t viewIndex,
	const Frustum* frustum,
	core::TaskExecutor* executor)
{
	ZoneScopedN("gfx::RecordDrawList");

	CommandEncoder encoder(cmd);
	GraphicsPipelineVariant bound{.blend = kOpaqueBlend};
	// the state the draw before set (none yet: a secondary command buffer starts without dynamic state)
	std::optional<CullMode> cullMode;
	std::optional<FrontFace> frontFace;
	std::optional<PushConstants> pushed;
	for (const auto& item : list.items)
	{
		if (frustum != nullptr && item.cullable && frustum->Excludes(item.bounds))
			continue;

		auto variant = item.variant;
		if (variant.specialization != 0 && executor != nullptr)
		{
			// the generic pipeline until the specialized one exists (its creation takes tens of milliseconds)
			auto request = pipeline.RequestGraphicsPipeline(variant);
			if (request.handle == nullptr)
			{
				if (request.create)
					executor->Submit({&request.create, 1});
				variant.specialization = 0;
			}
		}
		if (variant != bound)
		{
			bound = variant;
			pipeline.BindPipelineAuto(cmd, variant);
			// push constants outlive a pipeline bind with the same layout, as here, but variants switch rarely enough (the
			// draws are sorted by them) not to rely on it
			pushed.reset();
		}
		if (cullMode != item.cullMode)
		{
			cullMode = item.cullMode;
			encoder.SetCullMode(item.cullMode);
		}
		if (frontFace != item.frontFace)
		{
			frontFace = item.frontFace;
			encoder.SetFrontFace(item.frontFace);
		}

		pushConstants.viewId = viewIndex;
		pushConstants.drawRecord = item.record;
		// compared bytewise: both are copies of the caller's, padding included
		if (!pushed || std::memcmp(&*pushed, &pushConstants, sizeof(PushConstants)) != 0)
		{
			pushed = pushConstants;
			pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));
		}
		encoder.DrawIndexed(item.indexCount, item.instanceCount, item.firstIndex);
	}

	if (bound != GraphicsPipelineVariant{.blend = kOpaqueBlend})
		pipeline.BindPipelineAuto(cmd, {.blend = kOpaqueBlend});
}

} // namespace gfx
