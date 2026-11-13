// the Metal backend's device: an MTL::Device (the instance's, retained), one queue family of one queue for every queue
// type (compute and transfer alias graphics, see docs/backends-plan.md), and the pipeline
#include <rhi/metal/backend.h>

#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/application.h>
#include <core/profiling.h>

#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Device<kMetal>);

namespace device
{

// ray queries where the device traces from every stage and the application asks for them (opt in on macOS)
[[nodiscard]] static bool RayQueries(MTL::Device* device)
{
	return metal::RayQueriesEnabled() && device->supportsRaytracing() && device->supportsRaytracingFromRender();
}

} // namespace device

template <>
Device<kMetal>::Device(CreateDescType&& desc, const Instance<kMetal>& /*instance*/)
	: Object(std::forward<CreateDescType>(desc))
{
	ZoneScopedN("Device()");

	myDevice = reinterpret_cast<MTL::Device*>(GetPhysicalDevice())->retain();
	myAllocator = new metal::AllocatorObject{.device = myDevice};
	// timestamps where the device has the timestamp counter set and samples it where encoders begin and end (see
	// QueryPool): 64 bits of nanoseconds
	bool timestamps = false;
	NS::Array* sets = myDevice->counterSets();
	for (NS::UInteger setIt = 0; sets != nullptr && setIt < sets->count(); setIt++)
		timestamps = timestamps || sets->object<MTL::CounterSet>(setIt)->name()->isEqualToString(MTL::CommonCounterSetTimestamp);
	timestamps = timestamps && myDevice->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary);
	// a family each queue type, dedicated as Vulkan's backend takes them where a device has them (graphics, compute,
	// transfer: see InternalCreateQueues), a queue each
	for (uint32_t flags : std::array<uint32_t, 3>{
			 kQueueFamilyFlagBitsGraphics | kQueueFamilyFlagBitsCompute | kQueueFamilyFlagBitsTransfer,
			 kQueueFamilyFlagBitsCompute | kQueueFamilyFlagBitsTransfer,
			 kQueueFamilyFlagBitsTransfer})
		myQueueFamilyDescs.push_back(QueueFamilyDesc<kMetal>{
			.queueCount = 1,
			.flags = flags,
			.timestampValidBits = timestamps ? 64U : 0U,
		});
	std::println("\"{}\" is selected as primary graphics device (Metal)", myDevice->name()->utf8String());
	std::println("ray queries: {}", device::RayQueries(myDevice) ? "yes" : "no (the shaders traverse gfx's BVHs)");
}

template <>
void Device<kMetal>::Swap(Device& other) noexcept
{
	SuperType::Swap(other);
	std::swap(myDevice, other.myDevice);
	std::swap(myAllocator, other.myAllocator);
	std::swap(myQueueFamilyDescs, other.myQueueFamilyDescs);
	std::swap(myQueues, other.myQueues);
	std::swap(myPipeline, other.myPipeline);
	std::swap(myResources, other.myResources);
}

template <>
Device<kMetal>::Device(Device&& other) noexcept
{
	Swap(other);
}

template <>
Device<kMetal>& Device<kMetal>::operator=(Device<kMetal>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Device<kMetal>::~Device()
{
	ZoneScopedN("~Device()");

	if (!IsValid())
		return;

	myResources.clear();
	myPipeline = Pipeline<kMetal>{};
	myQueues.clear();
	if (myAllocator->instancePipeline != nullptr)
		myAllocator->instancePipeline->release();
	delete myAllocator;
	myDevice->release();
}

template <>
DeviceLimits Device<kMetal>::GetLimits() const
{
	// Apple's gpus' (the Metal feature set tables, Apple7 and up): argument buffers of tier 2 hold up to 500000 textures;
	// no draw count indirect in Metal (the renderer draws its commands' slots then, zeroed past the count, see
	// DeviceLimits::drawIndirectCountEmulated), an indirect draw's commands issued one by one (CommandEncoder::
	// DrawIndexedIndirect); timestamps in nanoseconds (counter sample buffers); ray queries where the device traces from
	// every stage and they're asked for (opt in on macOS, metal::RayQueriesEnabled)
	constexpr uint32_t kArgumentBufferTextureCount = 500000;
	constexpr uint32_t kMaxDrawIndirectCount = 1U << 24U;
	constexpr uint32_t kMaxImageDimension2d = 16384;
	constexpr uint32_t kMaxImageDimension3d = 2048;
	constexpr uint32_t kScratchAlignment = 256;
	return DeviceLimits{
		.maxStorageBufferRange = myDevice->maxBufferLength(),
		.maxPerStageSampledImages = kArgumentBufferTextureCount,
		.maxDrawIndirectCount = kMaxDrawIndirectCount,
		.maxImageDimension2d = kMaxImageDimension2d,
		.maxImageDimension3d = kMaxImageDimension3d,
		.timestampPeriod = 1.0F,
		.drawIndirectCount = false,
		.drawIndirectCountEmulated = false,
		.rayQuery = device::RayQueries(myDevice),
		.accelerationStructureScratchAlignment = kScratchAlignment};
}

template <>
Format Device<kMetal>::FindSupportedFormat(std::span<const Format> candidates, ImageTiling /*tiling*/, FormatFeature features) const
{
	// the first Metal has (no 24 bit color, no depth 24 stencil 8 on Apple's gpus) that can be what features ask: a block
	// compressed format no attachment, a color attachment a color format, a depth stencil attachment a depth format
	for (auto candidate : candidates)
	{
		if (candidate == Format::kUndefined || candidate == Format::kR8G8B8Unorm || candidate == Format::kB8G8R8Unorm ||
			candidate == Format::kD24UnormS8Uint)
			continue;
		const bool compressed = BlockOf(candidate).width > 1;
		const bool attachment = Any(features & (FormatFeature::kColorAttachment | FormatFeature::kDepthStencilAttachment));
		if (compressed && attachment)
			continue;
		if (Any(features & FormatFeature::kColorAttachment) && !HasColorComponent(candidate))
			continue;
		if (Any(features & FormatFeature::kDepthStencilAttachment) && !HasDepthComponent(candidate))
			continue;
		return candidate;
	}
	return Format::kUndefined;
}

template <>
std::string_view Device<kMetal>::GetName() const
{
	return myDevice->name()->utf8String();
}

template <>
void Device<kMetal>::WaitIdle() const
{
	ZoneScopedN("Device::WaitIdle");

	for (const auto& [type, queue] : myQueues)
	{
		auto access = queue->Read();
		for (const auto& [deviceQueue, syncInfo] : access->queues)
			deviceQueue.WaitIdle();
	}
}

template <>
void Device<kMetal>::InternalCreateQueues()
{
	ZoneScopedN("Device::InternalCreateQueues");

	// a queue of its own each queue type, in its family (see the constructor), with its own timeline: an MTLCommandQueue
	// each (Metal makes any number, and Apple's gpus run independent queues' work concurrently; no copy engine is
	// exposed, so the transfer queue's gain is uploads that don't wait behind the frames' work). Metal's hazard tracking
	// doesn't cross queues: their timelines' waits order them (Queue::Submit encodes a wait on another queue's)
	constexpr std::array kQueueTypes{
		std::pair{kQueueTypeGraphics, "Graphics"}, std::pair{kQueueTypeCompute, "Compute"}, std::pair{kQueueTypeTransfer, "Transfer"}};
	auto& queues = GetQueues();
	for (uint32_t familyIt = 0; familyIt < kQueueTypes.size(); familyIt++)
	{
		const auto [type, name] = kQueueTypes[familyIt];
		auto context = std::make_shared<QueueTimelineContext<kMetal>>(std::make_shared<QueueTimelineContextData<kMetal>>(
			Semaphore<kMetal>{SemaphoreCreateDesc<kMetal>{
				CreateDeviceObjectCreateDesc(std::format("{} Queue Timeline Semaphore", name)), SemaphoreType::kTimeline}},
			uint64_t{},
			uint32_t{},
			core::CircularContainer<QueueContext<kMetal>>{}));
		{
			auto data = context->Write();
			data->queues = std::vector<QueueContext<kMetal>>(1);
			data->queueFamilyIndex = familyIt;
			auto& [queue, syncInfo] = data->queues.FetchAdd();
			queue = Queue<kMetal>(QueueCreateDesc<kMetal>{
				CreateDeviceObjectCreateDesc(std::format("{} Queue 0", name)),
				0,
				familyIt,
				static_cast<uint32_t>(GetQueueFamilies()[familyIt].timestampValidBits > 0)});
		}
		queues.emplace(type, context);
	}
}

template <>
void Device<kMetal>::InternalCreatePipeline(std::vector<DescriptorPoolSize>&& descriptorPoolSizes)
{
	ZoneScopedN("Device::InternalCreatePipeline");

	myPipeline = Pipeline<kMetal>(PipelineCreateDesc<kMetal>{
		CreateDeviceObjectCreateDesc("Pipeline"),
		(std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / "pipeline.metal.cache").string(),
		std::move(descriptorPoolSizes)});
}

} // namespace rhi
