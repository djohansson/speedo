#include "drawlist.h"

#include <gfx/model.h>

#include <core/profiling.h>

#include <span>
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
	const std::function<bool(size_t material)>& transmissive,
	const std::function<uint16_t(size_t material)>& specialization)
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
				 .specialization = specialization ? (submesh.material >= 0 ? specialization(static_cast<size_t>(submesh.material))
																		   : static_cast<uint16_t>(SHADER_TYPES_LAYERS_EXTENDED))
												  : uint16_t{0}},
			.cullMode = doubleSided ? CullMode::kNone : CullMode::kBack,
			.materialSlot = ModelMaterialSlot(submesh.material),
			.jointBase = submesh.skin >= 0 ? skins[submesh.skin].jointBase : SHADER_TYPES_NOT_SKINNED,
			.morphTargetCount = submesh.morphTargetCount,
			.morphDeltaBase = submesh.morphDeltaBase,
			.morphFirstVertex = submesh.morphFirstVertex,
			.morphWeightBase = submesh.morphWeightBase};

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
	for (const auto& submesh : desc.submeshes)
		if (!blended(submesh) && phaseOf(submesh) == phase)
			add(submesh, kOpaqueBlend, 0);
	if (!twoPhases || phase == MainPassPhase::kTransmissive)
		for (const auto& submesh : desc.submeshes)
			if (blended(submesh))
				add(submesh, kTransparentBlend, kTransparentFragmentShader);

	return list;
}

DrawList BuildShadowDrawList(const Model& model, const std::function<bool(size_t material)>& transmissive)
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

void RecordDrawList(CommandBufferHandle cmd, Pipeline& pipeline, const DrawList& list, PushConstants pushConstants, uint16_t viewIndex)
{
	ZoneScopedN("gfx::RecordDrawList");

	CommandEncoder encoder(cmd);
	GraphicsPipelineVariant bound{.blend = kOpaqueBlend};
	for (const auto& item : list.items)
	{
		if (item.variant != bound)
		{
			bound = item.variant;
			pipeline.BindPipelineAuto(cmd, item.variant);
		}
		encoder.SetCullMode(item.cullMode);
		encoder.SetFrontFace(item.frontFace);

		pushConstants.viewAndMaterialId = (static_cast<uint32_t>(viewIndex) << SHADER_TYPES_MATERIAL_INDEX_BITS) | item.materialSlot;
		pushConstants.modelInstanceId = item.firstInstance;
		pushConstants.jointBase = item.jointBase;
		pushConstants.morphTargetCount = item.morphTargetCount;
		pushConstants.morphDeltaBase = item.morphDeltaBase;
		pushConstants.morphFirstVertex = item.morphFirstVertex;
		pushConstants.morphWeightBase = item.morphWeightBase;
		pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));
		encoder.DrawIndexed(item.indexCount, item.instanceCount, item.firstIndex);
	}

	if (bound != GraphicsPipelineVariant{.blend = kOpaqueBlend})
		pipeline.BindPipelineAuto(cmd, {.blend = kOpaqueBlend});
}

} // namespace gfx
