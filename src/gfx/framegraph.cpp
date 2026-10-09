#include "framegraph.h"

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <format>
#include <numeric>
#include <utility>

namespace gfx
{

using namespace rhi;

bool ResourceUse::Writes() const noexcept
{
	return Any(
		access & (Access::kShaderWrite | Access::kTransferWrite | Access::kHostWrite | Access::kColorAttachmentWrite |
				  Access::kDepthStencilAttachmentWrite));
}

ResourceUse ResourceUse::ColorAttachment()
{
	return {
		.layout = ImageLayout::kColorAttachment,
		.stages = PipelineStage::kColorAttachmentOutput,
		.access = Access::kColorAttachmentRead | Access::kColorAttachmentWrite};
}

ResourceUse ResourceUse::DepthAttachment()
{
	return {
		.layout = ImageLayout::kDepthStencilAttachment,
		.stages = PipelineStage::kEarlyFragmentTests | PipelineStage::kLateFragmentTests,
		.access = Access::kDepthStencilAttachmentRead | Access::kDepthStencilAttachmentWrite};
}

ResourceUse ResourceUse::Sampled(PipelineStage stages)
{
	return {.layout = ImageLayout::kShaderReadOnly, .stages = stages, .access = Access::kShaderRead};
}

ResourceUse ResourceUse::Storage(PipelineStage stages, Access access)
{
	return {.layout = ImageLayout::kGeneral, .stages = stages, .access = access};
}

ResourceUse ResourceUse::TransferSource()
{
	return {.layout = ImageLayout::kTransferSource, .stages = PipelineStage::kTransfer, .access = Access::kTransferRead};
}

ResourceUse ResourceUse::TransferDestination()
{
	return {.layout = ImageLayout::kTransferDestination, .stages = PipelineStage::kTransfer, .access = Access::kTransferWrite};
}

ResourceUse ResourceUse::HostRead()
{
	return {.layout = ImageLayout::kUndefined, .stages = PipelineStage::kHost, .access = Access::kHostRead};
}

ResourceUse ResourceUse::Present()
{
	return {.layout = ImageLayout::kPresent, .stages = PipelineStage::kBottomOfPipe, .access = Access::kNone};
}

void FrameGraph::PassBuilder::Use(ImageId image, const ResourceUse& use)
{
	ENSURE(image.index < myGraph.myResources.size() && myGraph.myResources[image.index].image);
	myGraph.myPasses[myPass].uses.emplace_back(image.index, use);
}

void FrameGraph::PassBuilder::Use(BufferId buffer, const ResourceUse& use)
{
	ENSURE(buffer.index < myGraph.myResources.size() && !myGraph.myResources[buffer.index].image);
	myGraph.myPasses[myPass].uses.emplace_back(buffer.index, use);
}

void FrameGraph::PassBuilder::ColorAttachment(ImageId image, LoadOp load, StoreOp store, const ClearValue& clear)
{
	auto& pass = myGraph.myPasses[myPass];
	// color attachments come before the depth attachment
	ENSURE(pass.attachments.empty() || !Any(myGraph.myResources[pass.attachments.back().resource].imageDesc.aspect & ImageAspect::kDepth));
	Use(image, ResourceUse::ColorAttachment());
	pass.attachments.push_back({.resource = image.index, .load = load, .store = store, .clear = clear});
}

void FrameGraph::PassBuilder::DepthAttachment(ImageId image, LoadOp load, StoreOp store, const ClearValue& clear)
{
	Use(image, ResourceUse::DepthAttachment());
	myGraph.myPasses[myPass].attachments.push_back({.resource = image.index, .load = load, .store = store, .clear = clear});
}

void FrameGraph::PassBuilder::Contents(SubpassContents contents)
{
	myGraph.myPasses[myPass].contents = contents;
}

void FrameGraph::PassBuilder::SideEffects()
{
	myGraph.myPasses[myPass].sideEffects = true;
}

void FrameGraph::PassBuilder::Wrap(WrapFunction wrap)
{
	myGraph.myPasses[myPass].wrap = std::move(wrap);
}

FrameGraph::~FrameGraph()
{
	// what is placed in the blocks goes before them
	for (auto& pass : myPasses)
		pass.renderTarget.reset();
	for (auto& resource : myResources)
	{
		resource.transientView.reset();
		resource.transientImage.reset();
		resource.transientBuffer.reset();
	}
	myBlocks.clear();
}

FrameGraph::ImageId FrameGraph::CreateImage(ImageDesc desc)
{
	ENSURE(!myCompiled);
	auto& resource = myResources.emplace_back();
	resource.name = desc.name;
	resource.imageDesc = std::move(desc);
	return {static_cast<uint32_t>(myResources.size() - 1)};
}

FrameGraph::BufferId FrameGraph::CreateBuffer(BufferDesc desc)
{
	ENSURE(!myCompiled);
	auto& resource = myResources.emplace_back();
	resource.name = desc.name;
	resource.image = false;
	resource.bufferDesc = std::move(desc);
	return {static_cast<uint32_t>(myResources.size() - 1)};
}

FrameGraph::ImageId FrameGraph::ImportImage(std::string name, ImageAspect aspect)
{
	ENSURE(!myCompiled);
	auto& resource = myResources.emplace_back();
	resource.name = std::move(name);
	resource.imported = true;
	resource.imageDesc.aspect = aspect;
	return {static_cast<uint32_t>(myResources.size() - 1)};
}

FrameGraph::BufferId FrameGraph::ImportBuffer(std::string name)
{
	ENSURE(!myCompiled);
	auto& resource = myResources.emplace_back();
	resource.name = std::move(name);
	resource.image = false;
	resource.imported = true;
	return {static_cast<uint32_t>(myResources.size() - 1)};
}

FrameGraph::PassId FrameGraph::AddPass(std::string name, const std::function<void(PassBuilder& builder)>& setup, ExecuteFunction execute)
{
	ENSURE(!myCompiled);
	auto& pass = myPasses.emplace_back();
	pass.name = std::move(name);
	pass.execute = std::move(execute);
	PassBuilder builder(*this, static_cast<uint32_t>(myPasses.size() - 1));
	setup(builder);
	return {static_cast<uint32_t>(myPasses.size() - 1)};
}

void FrameGraph::SetEnabled(PassId pass, bool enabled)
{
	myPasses.at(pass.index).enabled = enabled;
}

void FrameGraph::Bind(ImageId image, IRenderTarget& target, uint32_t index)
{
	auto& resource = myResources.at(image.index);
	ENSURE(resource.imported && resource.image);
	resource.boundImage = std::pair{&target, index};
}

void FrameGraph::Bind(ImageId image, Image& bound)
{
	auto& resource = myResources.at(image.index);
	ENSURE(resource.imported && resource.image);
	resource.boundImage = &bound;
}

void FrameGraph::Bind(BufferId buffer, Buffer& bound)
{
	auto& resource = myResources.at(buffer.index);
	ENSURE(resource.imported && !resource.image);
	resource.boundBuffer = &bound;
}

Image& FrameGraph::GetImage(ImageId image) const
{
	const auto& resource = myResources.at(image.index);
	if (resource.imported)
	{
		ENSUREF(std::holds_alternative<Image*>(resource.boundImage), "{} isn't bound as an image", resource.name);
		return *std::get<Image*>(resource.boundImage);
	}
	ENSUREF(resource.transientImage, "{} isn't used by any pass", resource.name);
	return *resource.transientImage;
}

const ImageView& FrameGraph::GetView(ImageId image) const
{
	const auto& resource = myResources.at(image.index);
	ENSUREF(resource.transientView, "{} isn't a transient used by any pass", resource.name);
	return *resource.transientView;
}

Buffer& FrameGraph::GetBuffer(BufferId buffer) const
{
	const auto& resource = myResources.at(buffer.index);
	if (resource.imported)
	{
		ENSUREF(resource.boundBuffer, "{} isn't bound", resource.name);
		return *resource.boundBuffer;
	}
	ENSUREF(resource.transientBuffer, "{} isn't used by any pass", resource.name);
	return *resource.transientBuffer;
}

void FrameGraph::InternalCull()
{
	// backwards: a pass is needed if it has side effects, writes an imported resource, or writes what a needed pass after
	// it uses
	std::vector<bool> needed(myResources.size(), false);
	for (auto passIt = myPasses.rbegin(); passIt != myPasses.rend(); ++passIt)
	{
		auto& pass = *passIt;
		pass.live = pass.sideEffects;
		for (const auto& [resource, use] : pass.uses)
			pass.live |= use.Writes() && (myResources[resource].imported || needed[resource]);
		if (pass.live)
			for (const auto& [resource, use] : pass.uses)
				needed[resource] = true;
	}

	// the live passes' resources: their lifetimes, and the usages of the transients
	for (uint32_t passIt = 0; passIt < myPasses.size(); passIt++)
	{
		const auto& pass = myPasses[passIt];
		if (!pass.live)
			continue;
		for (const auto& [index, use] : pass.uses)
		{
			auto& resource = myResources[index];
			resource.firstPass = std::min(resource.firstPass, passIt);
			resource.lastPass = std::max(resource.lastPass, passIt);
			if (resource.image)
			{
				switch (use.layout)
				{
				case ImageLayout::kColorAttachment: resource.imageUsage |= ImageUsage::kColorAttachment; break;
				case ImageLayout::kDepthStencilAttachment: resource.imageUsage |= ImageUsage::kDepthStencilAttachment; break;
				case ImageLayout::kShaderReadOnly: resource.imageUsage |= ImageUsage::kSampled; break;
				case ImageLayout::kGeneral: resource.imageUsage |= ImageUsage::kStorage; break;
				case ImageLayout::kTransferSource: resource.imageUsage |= ImageUsage::kTransferSource; break;
				case ImageLayout::kTransferDestination: resource.imageUsage |= ImageUsage::kTransferDestination; break;
				default: break;
				}
			}
			else
			{
				if (Any(use.access & (Access::kShaderRead | Access::kShaderWrite)))
					resource.bufferUsage |= BufferUsage::kStorage;
				if (Any(use.access & Access::kTransferRead))
					resource.bufferUsage |= BufferUsage::kTransferSource;
				if (Any(use.access & Access::kTransferWrite))
					resource.bufferUsage |= BufferUsage::kTransferDestination;
			}
		}
	}
	// a transient's contents don't outlive the frame: its first use must write it, not read or load it
	for (uint32_t passIt = 0; passIt < myPasses.size(); passIt++)
	{
		const auto& pass = myPasses[passIt];
		if (!pass.live)
			continue;
		for (const auto& [index, use] : pass.uses)
			ENSUREF(
				myResources[index].imported || myResources[index].firstPass != passIt || use.Writes(),
				"{}: the first use of transient {} reads it",
				pass.name,
				myResources[index].name);
		for (const auto& attachment : pass.attachments)
			ENSUREF(
				myResources[attachment.resource].imported || myResources[attachment.resource].firstPass != passIt ||
					attachment.load != LoadOp::kLoad,
				"{}: the first use of transient {} loads it",
				pass.name,
				myResources[attachment.resource].name);
	}

	for (auto& resource : myResources)
		if (resource.image && resource.imageDesc.mipChain)
			// mips are blitted from the level above (see Image::GenerateMips)
			resource.imageUsage |= ImageUsage::kTransferSource | ImageUsage::kTransferDestination;
}

void FrameGraph::InternalCreateTransients(Device& device)
{
	ZoneScopedN("FrameGraph::InternalCreateTransients");

	// what is placed in the blocks goes before them
	for (auto& pass : myPasses)
		pass.renderTarget.reset();
	for (auto& resource : myResources)
	{
		resource.transientView.reset();
		resource.transientImage.reset();
		resource.transientBuffer.reset();
		resource.overlaps.clear();
		resource.state = {};
	}
	myBlocks.clear();

	// the transients the live passes use, with what they need of the memory
	std::vector<uint32_t> transients;
	std::vector<ImageCreateDesc> imageDescs(myResources.size());
	std::vector<BufferCreateDesc> bufferDescs(myResources.size());
	std::vector<MemoryRequirements> requirements(myResources.size());
	for (uint32_t index = 0; index < myResources.size(); index++)
	{
		auto& resource = myResources[index];
		if (resource.imported || resource.firstPass == UINT32_MAX)
			continue;

		transients.push_back(index);
		if (resource.image)
		{
			const auto& desc = resource.imageDesc;
			auto extent = desc.extent.width > 0 ? desc.extent : myExtent;
			std::vector<ImageMipLevelDesc> mipLevels{{.extent = extent}};
			while (desc.mipChain && (extent.width > 1 || extent.height > 1))
			{
				extent = {.width = std::max(extent.width / 2, 1U), .height = std::max(extent.height / 2, 1U)};
				mipLevels.push_back({.extent = extent});
			}
			imageDescs[index] = ImageCreateDesc{
				device.CreateDeviceObjectCreateDesc(resource.name),
				std::move(mipLevels),
				desc.format,
				ImageTiling::kOptimal,
				resource.imageUsage,
				MemoryProperty::kDeviceLocal,
				desc.aspect,
				ImageLayout::kUndefined};
			requirements[index] = Image::GetMemoryRequirements(device, imageDescs[index]);
		}
		else
		{
			const auto& desc = resource.bufferDesc;
			bufferDescs[index] = BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc(resource.name),
				std::max<uint64_t>(desc.sizeOf ? desc.sizeOf(myExtent) : desc.size, 4),
				resource.bufferUsage,
				MemoryProperty::kDeviceLocal};
			requirements[index] = Buffer::GetMemoryRequirements(device, bufferDescs[index]);
		}
		resource.size = requirements[index].size;
	}

	// placement: images and buffers in blocks of their own (so that no buffer-image granularity applies between them),
	// per set of memory types they allow. largest first, each at the lowest offset where it overlaps none of the
	// transients placed there whose lifetimes overlap its own.
	auto lifetimesOverlap = [this](uint32_t a, uint32_t b)
	{ return !(myResources[a].lastPass < myResources[b].firstPass || myResources[b].lastPass < myResources[a].firstPass); };
	auto bytesOverlap = [this](uint32_t a, uint32_t b)
	{
		const auto& ra = myResources[a];
		const auto& rb = myResources[b];
		return ra.offset < rb.offset + rb.size && rb.offset < ra.offset + ra.size;
	};

	std::ranges::sort(transients, [&](uint32_t a, uint32_t b) { return requirements[a].size > requirements[b].size; });
	struct Block
	{
		bool image;
		uint32_t memoryTypeBits;
		uint64_t size = 0;
		uint64_t alignment = 1;
		std::vector<uint32_t> placed;
	};
	std::vector<Block> blocks;
	myUnaliasedTransientMemorySize = 0;
	for (auto index : transients)
	{
		auto& resource = myResources[index];
		const auto& requirement = requirements[index];
		myUnaliasedTransientMemorySize += requirement.size;

		auto blockIt = std::ranges::find_if(
			blocks,
			[&](const Block& block) { return block.image == resource.image && block.memoryTypeBits == requirement.memoryTypeBits; });
		if (blockIt == blocks.end())
			blockIt = blocks.insert(blocks.end(), Block{.image = resource.image, .memoryTypeBits = requirement.memoryTypeBits});
		auto& block = *blockIt;

		// candidates: the start, and the end of every placed transient whose lifetime overlaps
		auto alignUp = [&requirement](uint64_t offset) { return (offset + requirement.alignment - 1) / requirement.alignment * requirement.alignment; };
		std::vector<uint64_t> candidates{0};
		for (auto other : block.placed)
			if (lifetimesOverlap(index, other))
				candidates.push_back(alignUp(myResources[other].offset + myResources[other].size));
		std::ranges::sort(candidates);
		for (auto candidate : candidates)
		{
			resource.offset = candidate;
			if (std::ranges::none_of(block.placed, [&](uint32_t other) { return lifetimesOverlap(index, other) && bytesOverlap(index, other); }))
				break;
		}
		resource.block = static_cast<uint32_t>(blockIt - blocks.begin());
		block.placed.push_back(index);
		block.size = std::max(block.size, resource.offset + resource.size);
		block.alignment = std::max(block.alignment, requirement.alignment);
	}

	myTransientMemorySize = 0;
	myBlocks.reserve(blocks.size());
	for (const auto& block : blocks)
	{
		myTransientMemorySize += block.size;
		myBlocks.emplace_back(
			device.GetAllocator(),
			MemoryRequirements{.size = block.size, .alignment = block.alignment, .memoryTypeBits = block.memoryTypeBits},
			MemoryProperty::kDeviceLocal,
			block.image ? "FrameGraph Images" : "FrameGraph Buffers");
	}

	for (auto index : transients)
	{
		auto& resource = myResources[index];
		for (auto other : blocks[resource.block].placed)
			if (bytesOverlap(index, other))
				resource.overlaps.push_back(other);

		if (resource.image)
		{
			auto format = imageDescs[index].format;
			resource.transientImage = std::make_shared<Image>(std::move(imageDescs[index]), myBlocks[resource.block], resource.offset);
			// for sampling or storage (views need one of those usages, or an attachment's): the depth of a depth stencil image
			if (Any(resource.imageUsage & (ImageUsage::kSampled | ImageUsage::kStorage)))
			{
				auto aspect = Any(resource.imageDesc.aspect & ImageAspect::kDepth) ? ImageAspect::kDepth : resource.imageDesc.aspect;
				resource.transientView = std::make_unique<ImageView>(ImageViewCreateDesc{
					device.CreateDeviceObjectCreateDesc(std::format("{} View", resource.name)), *resource.transientImage, format, aspect});
			}
		}
		else
			resource.transientBuffer = std::make_unique<Buffer>(std::move(bufferDescs[index]), myBlocks[resource.block], resource.offset);
	}

	// the raster passes over transients get a render target each, shared by those with the same attachments
	for (uint32_t passIt = 0; passIt < myPasses.size(); passIt++)
	{
		auto& pass = myPasses[passIt];
		if (!pass.live || pass.attachments.empty() || myResources[pass.attachments.front().resource].imported)
			continue;

		auto same = [&pass](const Pass& other)
		{
			return other.renderTarget && std::ranges::equal(
				other.attachments, pass.attachments, [](const Attachment& a, const Attachment& b) { return a.resource == b.resource; });
		};
		if (auto it = std::ranges::find_if(std::span(myPasses).first(passIt), same); it != myPasses.begin() + passIt)
		{
			pass.renderTarget = it->renderTarget;
			continue;
		}

		std::vector<std::shared_ptr<Image>> images;
		for (const auto& attachment : pass.attachments)
		{
			const auto& resource = myResources[attachment.resource];
			ENSUREF(!resource.imported, "{}: attachments are all transient or all imported", pass.name);
			images.push_back(resource.transientImage);
		}
		pass.renderTarget = std::make_shared<RenderImageSet>(std::move(images));
	}
}

bool FrameGraph::Compile(Device& device, Extent2d extent)
{
	ZoneScopedN("FrameGraph::Compile");

	if (!myCompiled)
	{
		InternalCull();
		myCompiled = true;
	}
	else if (extent.width == myExtent.width && extent.height == myExtent.height)
		return false;

	myExtent = extent;
	InternalCreateTransients(device);
	return true;
}

ImageLayout FrameGraph::InternalLayout(const Resource& resource) const
{
	if (!resource.imported)
		return resource.transientImage->GetDesc().layout;
	if (const auto* bound = std::get_if<std::pair<IRenderTarget*, uint32_t>>(&resource.boundImage))
		return bound->first->GetLayout(bound->second);
	ENSUREF(std::holds_alternative<Image*>(resource.boundImage), "{} isn't bound", resource.name);
	return std::get<Image*>(resource.boundImage)->GetDesc().layout;
}

void FrameGraph::InternalTransition(CommandBufferHandle cmd, Resource& resource, ImageLayout layout)
{
	if (!resource.imported)
		resource.transientImage->Transition(cmd, layout, resource.imageDesc.aspect);
	else if (auto* bound = std::get_if<std::pair<IRenderTarget*, uint32_t>>(&resource.boundImage))
		bound->first->Transition(cmd, layout, resource.imageDesc.aspect, bound->second);
	else
		std::get<Image*>(resource.boundImage)->Transition(cmd, layout, resource.imageDesc.aspect);
}

void FrameGraph::InternalBarriers(CommandBufferHandle cmd, Pass& pass, uint32_t /*passIndex*/)
{
	auto srcStages = PipelineStage::kNone;
	auto srcAccess = Access::kNone;
	auto dstStages = PipelineStage::kNone;
	auto dstAccess = Access::kNone;
	std::vector<std::pair<uint32_t, ImageLayout>> transitions;

	auto wait = [&](PipelineStage stages, Access access, const ResourceUse& use)
	{
		if (stages == PipelineStage::kNone)
			return;
		srcStages |= stages;
		srcAccess |= access;
		dstStages |= use.stages;
		dstAccess |= use.access;
	};

	for (const auto& [index, use] : pass.uses)
	{
		auto& resource = myResources[index];
		auto prior = resource.state;
		bool layoutChange = resource.image && use.layout != ImageLayout::kUndefined && InternalLayout(resource) != use.layout;

		if (!resource.imported && !resource.usedThisFrame)
		{
			// the first use this frame: after whatever last used its memory (it, or a transient over the same bytes), whose
			// contents it doesn't need (see ImageDesc)
			resource.usedThisFrame = true;
			for (auto other : resource.overlaps)
			{
				const auto& state = myResources[other].state;
				wait(state.writeStages | state.readStages, state.writeAccess, use);
			}
			if (resource.image)
			{
				resource.transientImage->Discard();
				layoutChange = use.layout != ImageLayout::kUndefined;
			}
			prior = {};
		}

		if (layoutChange)
		{
			// the transition waits for its old layout's uses, and makes the image available to its new layout's
			transitions.emplace_back(index, use.layout);
			prior = {};
		}
		else if (use.Writes())
			wait(prior.writeStages | prior.readStages, prior.writeAccess, use);
		else if (prior.writeStages != PipelineStage::kNone &&
				 ((use.stages & prior.readStages) != use.stages || (use.access & prior.readAccess) != use.access))
			wait(prior.writeStages, prior.writeAccess, use);

		if (use.Writes())
			resource.state = {.writeStages = use.stages, .writeAccess = use.access};
		else
		{
			resource.state = prior;
			resource.state.readStages |= use.stages;
			resource.state.readAccess |= use.access;
		}
	}

	if (srcStages != PipelineStage::kNone)
		CommandEncoder(cmd).Barrier(srcStages, srcAccess, dstStages, dstAccess);
	for (auto [index, layout] : transitions)
		InternalTransition(cmd, myResources[index], layout);
}

void FrameGraph::EnableTimings([[maybe_unused]] Device& device, [[maybe_unused]] uint32_t frameCount)
{
	// a profiling aid: only in builds that profile (without it, Execute writes no timestamps)
#if (SPEEDO_PROFILING_LEVEL > 0)
	myTimestampPeriod = device.GetLimits().timestampPeriod;
	myTimingPools.clear();
	for (uint32_t frame = 0; frame < frameCount; frame++)
		myTimingPools.emplace_back(QueryPoolCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("FrameGraph Timings {}", frame)),
			static_cast<uint32_t>(std::max<size_t>(myPasses.size(), 1) * 2)});
	myTimedPasses.assign(frameCount, {});
#endif
}

std::vector<FrameGraph::PassTiming> FrameGraph::ReadTimings(uint32_t frame) const
{
	if (frame >= myTimingPools.size() || myTimedPasses[frame].empty())
		return {};
	const auto& passes = myTimedPasses[frame];
	std::vector<uint64_t> timestamps(passes.size() * 2);
	if (!myTimingPools[frame].Read(0, timestamps))
		return {};
	// only plausible pairs: some backends only write timestamps where their own command encoders begin or end (Metal),
	// so others are 0 or repeated
	std::vector<PassTiming> timings;
	for (size_t passIt = 0; passIt < passes.size(); passIt++)
	{
		auto begin = timestamps[passIt * 2];
		auto end = timestamps[(passIt * 2) + 1];
		if (begin == 0 || end < begin)
			continue;
		timings.push_back(
			{.name = myPasses[passes[passIt]].name, .milliseconds = static_cast<double>(end - begin) * myTimestampPeriod * 1e-6});
	}
	return timings;
}

void FrameGraph::Execute(CommandBufferHandle cmd, std::optional<uint32_t> timingFrame)
{
	ZoneScopedN("FrameGraph::Execute");

	ENSURE(myCompiled);

	for (auto& resource : myResources)
		resource.usedThisFrame = false;

	// the timestamps of this frame index: written in the order the passes run
	const QueryPool* timing = timingFrame && *timingFrame < myTimingPools.size() ? &myTimingPools[*timingFrame] : nullptr;
	std::vector<uint32_t>* timedPasses = timing != nullptr ? &myTimedPasses[*timingFrame] : nullptr;
	if (timing != nullptr)
	{
		timing->Reset(cmd, 0, static_cast<uint32_t>(myPasses.size() * 2));
		timedPasses->clear();
	}

	for (uint32_t passIt = 0; passIt < myPasses.size(); passIt++)
	{
		auto& pass = myPasses[passIt];
		if (!pass.live || !pass.enabled)
			continue;

		ZoneScoped;
		ZoneName(pass.name.c_str(), pass.name.size());

		// outside the pass's render target (one begun for secondary command buffers can hold no timestamps)
		auto timed = static_cast<uint32_t>(timedPasses != nullptr ? timedPasses->size() : 0);
		if (timing != nullptr)
		{
			timing->WriteTimestamp(cmd, timed * 2);
			timedPasses->push_back(passIt);
		}
		if (pass.wrap)
			pass.wrap(cmd, [this, cmd, &pass, passIt] { InternalRecord(cmd, pass, passIt); });
		else
			InternalRecord(cmd, pass, passIt);
		if (timing != nullptr)
			timing->WriteTimestamp(cmd, (timed * 2) + 1);
	}
}

void FrameGraph::InternalRecord(CommandBufferHandle cmd, Pass& pass, uint32_t passIndex)
{
	InternalBarriers(cmd, pass, passIndex);

	PassContext context{.graph = *this, .cmd = cmd};
	if (pass.attachments.empty())
	{
		pass.execute(context);
		return;
	}

	// the render target: the pass's own, or the one the imported attachments are bound to
	IRenderTarget* target = pass.renderTarget.get();
	std::vector<uint32_t> indices(pass.attachments.size());
	if (target == nullptr)
		for (size_t attachmentIt = 0; attachmentIt < pass.attachments.size(); attachmentIt++)
		{
			const auto& resource = myResources[pass.attachments[attachmentIt].resource];
			const auto* bound = std::get_if<std::pair<IRenderTarget*, uint32_t>>(&resource.boundImage);
			ENSUREF(bound && (target == nullptr || target == bound->first), "{}: attachments of one render target", pass.name);
			target = bound->first;
			indices[attachmentIt] = bound->second;
		}
	else
		std::iota(indices.begin(), indices.end(), 0U);

	for (size_t attachmentIt = 0; attachmentIt < pass.attachments.size(); attachmentIt++)
	{
		const auto& attachment = pass.attachments[attachmentIt];
		const auto& resource = myResources[attachment.resource];
		bool depth = Any(resource.imageDesc.aspect & ImageAspect::kDepth);
		auto index = indices[attachmentIt];
		target->SetLoadOp(attachment.load, index, depth ? attachment.load : LoadOp{});
		target->SetStoreOp(attachment.store, index, depth ? attachment.store : StoreOp{});
		target->SetClearValue(attachment.clear, index);
		// in its layout already (see InternalBarriers): updates the render target's attachment from the image's
		target->Transition(
			cmd, depth ? ImageLayout::kDepthStencilAttachment : ImageLayout::kColorAttachment, resource.imageDesc.aspect, index);
	}

	context.target = target;
	context.beginInfo = &target->Begin(cmd, pass.contents);
	pass.execute(context);
	target->End(cmd);
}

} // namespace gfx
