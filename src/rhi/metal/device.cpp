// the Metal backend's device: an MTL::Device (the instance's, retained), one queue family of one queue for every queue
// type (compute and transfer alias graphics, see docs/backends-plan.md), and the pipeline
#include <rhi/metal/backend.h>

#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/application.h>
#include <core/profiling.h>

#include <filesystem>
#include <format>
#include <memory>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Device<kMetal>);

template <>
Device<kMetal>::Device(CreateDescType&& desc, const Instance<kMetal>& /*instance*/)
	: Object(std::forward<CreateDescType>(desc))
{
	ZoneScopedN("Device()");

	myDevice = reinterpret_cast<MTL::Device*>(GetPhysicalDevice())->retain();
	myQueueFamilyDescs.push_back(QueueFamilyDesc<kMetal>{
		.queueCount = 1,
		.flags = kQueueFamilyFlagBitsGraphics | kQueueFamilyFlagBitsCompute | kQueueFamilyFlagBitsTransfer,
		.timestampValidBits = 0, // timestamps come with the gpu profiler (counter sample buffers), step 7
	});
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
	myDevice->release();
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

	auto& queues = GetQueues();
	auto graphicsContext = std::make_shared<QueueTimelineContext<kMetal>>(std::make_shared<QueueTimelineContextData<kMetal>>(
		Semaphore<kMetal>{SemaphoreCreateDesc<kMetal>{CreateDeviceObjectCreateDesc("Graphics Queue Timeline Semaphore"), SemaphoreType::kTimeline}},
		uint64_t{},
		uint32_t{},
		core::CircularContainer<QueueContext<kMetal>>{}));
	{
		auto graphics = graphicsContext->Write();
		graphics->queues = std::vector<QueueContext<kMetal>>(1);
		graphics->queueFamilyIndex = 0;
		auto& [queue, syncInfo] = graphics->queues.FetchAdd();
		queue = Queue<kMetal>(QueueCreateDesc<kMetal>{CreateDeviceObjectCreateDesc("Graphics Queue 0"), 0, 0, 0});
	}
	// one queue to start: the compute and transfer types share its context (the whole ConcurrentAccess, so that locking an
	// alias locks the same mutex)
	queues.emplace(kQueueTypeGraphics, graphicsContext);
	queues.emplace(kQueueTypeCompute, graphicsContext);
	queues.emplace(kQueueTypeTransfer, graphicsContext);
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
