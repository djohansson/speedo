#include "graphicsqueue.h"

#include <core/profiling.h>

#include <algorithm>
#include <utility>

namespace gfx
{

using namespace rhi;

// hands `resource` to a graphics queue submission that waits for all graphics work submitted so far, and releases it
// from that submission's timeline callback, i.e. once the gpu can no longer be using it. call on the draw thread.
void RetireAfterGraphicsWork(QueueTimelineContextData& graphics, std::shared_ptr<void> resource)
{
	if (!resource)
		return;

	auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();

	auto cmd = graphicsQueue.GetPool().Commands();
	cmd.End();

	std::vector<core::TaskHandle> callbacks;
	callbacks.emplace_back(core::CreateTask([resource = std::move(resource)] {}).handle);

	graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {PipelineStage::kAllCommands},
		.waitSemaphoreValues = {graphics.timeline},
		.signalSemaphores = {graphics.semaphore},
		.signalSemaphoreValues = {++graphics.timeline},
		.callbacks = std::move(callbacks),});

	graphicsSubmits |= graphicsQueue.Submit();
}

// writes data to buffer at offset, in a submission on the graphics queue that is ordered after all graphics work
// submitted so far, and before all that follows. call on the draw thread.
void UpdateBufferOnGraphics(
	QueueTimelineContextData& graphics, const Buffer& buffer, uint64_t offset, std::span<const std::byte> data)
{
	if (data.empty())
		return;

	auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();

	auto cmd = graphicsQueue.GetPool().Commands();
	{
		GPU_SCOPE(cmd, graphicsQueue, UpdateBuffer); //NOLINT(bugprone-suspicious-stringview-data-usage)

		CommandEncoder encoder(cmd);

		// after the frames in flight have read the old data
		encoder.Barrier(PipelineStage::kAllCommands, Access::kShaderRead, PipelineStage::kTransfer, Access::kTransferWrite);

		// UpdateBuffer takes 64 kB at most
		constexpr size_t kMaxUpdateSize = 65536;
		for (size_t chunk = 0; chunk < data.size(); chunk += kMaxUpdateSize)
			encoder.UpdateBuffer(buffer, offset + chunk, data.subspan(chunk, std::min(kMaxUpdateSize, data.size() - chunk)));

		encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kAllCommands, Access::kShaderRead);
	}
	cmd.End();

	graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {PipelineStage::kAllCommands},
		.waitSemaphoreValues = {graphics.timeline},
		.signalSemaphores = {graphics.semaphore},
		.signalSemaphoreValues = {++graphics.timeline},});

	graphicsSubmits |= graphicsQueue.Submit();
}

// in a graphics queue submission that waits for the uploads, acquires the uploaded resources for the graphics queue
// family and transitions the images to a shader readable layout. once that has executed, has the draw thread call
// bind (with the graphics queue context). call on the draw thread.
void TransitionThenBind(
	RHI& rhi,
	QueueTimelineContextData& graphics,
	const Uploads& uploads,
	std::function<void(QueueTimelineContextData&)> bind)
{
	auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();

	// what to wait for: all graphics work so far, and the latest upload on each other semaphore. uploads on the graphics
	// semaphore (the transfer queue type aliases the graphics queue's context) are covered by the first.
	QueueDeviceSyncInfo syncInfo{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {PipelineStage::kAllCommands},
		.waitSemaphoreValues = {graphics.timeline},};
	std::vector<const Semaphore*> waitSemaphores{&graphics.semaphore};
	auto waitFor = [&](const Upload& upload)
	{
		ENSURE(upload.semaphore != nullptr);
		auto semaIt = std::ranges::find(waitSemaphores, upload.semaphore);
		auto index = static_cast<size_t>(semaIt - waitSemaphores.begin());
		if (semaIt == waitSemaphores.end())
		{
			waitSemaphores.push_back(upload.semaphore);
			syncInfo.waitSemaphores.emplace_back(*upload.semaphore);
			syncInfo.waitDstStageMasks.emplace_back(PipelineStage::kAllCommands);
			syncInfo.waitSemaphoreValues.emplace_back(upload.value);
		}
		else if (index > 0)
		{
			syncInfo.waitSemaphoreValues[index] = std::max(syncInfo.waitSemaphoreValues[index], upload.value);
		}
	};

	auto cmd = graphicsQueue.GetPool().Commands();
	{
		GPU_SCOPE(cmd, graphicsQueue, Transition); //NOLINT(bugprone-suspicious-stringview-data-usage)

		CommandEncoder encoder(cmd);
		for (const auto& [buffer, upload] : uploads.buffers)
		{
			waitFor(upload);
			encoder.AcquireOwnership(
				*buffer,
				upload.queueFamilyIndex,
				graphics.queueFamilyIndex,
				PipelineStage::kAllCommands,
				Access::kShaderRead | Access::kIndexRead);
		}

		for (const auto& [image, upload] : uploads.images)
		{
			waitFor(upload);
			// in the upload's layout, which the transition below starts from
			encoder.AcquireOwnership(
				*image, upload.queueFamilyIndex, graphics.queueFamilyIndex, PipelineStage::kAllCommands, Access::kShaderRead);
			image->Transition(cmd, ImageLayout::kShaderReadOnly);
		}
	}
	cmd.End();

	auto [transitionDoneTask, transitionDoneFuture] = core::CreateTask([&rhi, bind = std::move(bind)]
	{
		auto [bindTask, bindFuture] = core::CreateTask<QueueTimelineContextData*>(
			[bind](QueueTimelineContextData* graphics) { bind(*graphics); });
		rhi.drawCalls.enqueue(bindTask);
	});

	syncInfo.signalSemaphores = {graphics.semaphore};
	syncInfo.signalSemaphoreValues = {++graphics.timeline};
	syncInfo.callbacks.emplace_back(transitionDoneTask);
	graphicsQueue.EnqueueSubmit(std::move(syncInfo));

	graphicsSubmits |= graphicsQueue.Submit();
}

} // namespace gfx
