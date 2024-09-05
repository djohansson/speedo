#pragma once

#include "capi.h"
#include "command.h"
#include "device.h"
#include "instance.h"
#include "pipeline.h"
#include "queue.h"
#include "renderimageset.h"
#include "semaphore.h"
#include "shaders/capi.h"
#include "types.h"
#include "window.h"

#include <core/utils.h>

#include <memory>
#include <vector>
#include <tuple>

template <GraphicsApi G>
struct Rhi
{
	std::shared_ptr<Instance<G>> instance;
	std::shared_ptr<Device<G>> device;
	std::unique_ptr<Pipeline<G>> pipeline;

	UnorderedMap<QueueType, QueueTimelineContext<G>> queues;
	UnorderedMap<WindowHandle, Window<G>> windows;

	// temp until we have a proper resource manager
	UnorderedMap<std::string, PipelineLayoutHandle<G>> pipelineLayouts;
	//std::unique_ptr<ResourceContext<G>> resources;

	std::shared_ptr<RenderImageSet<G>> renderImageSet;

	std::unique_ptr<Buffer<G>> materials;
	std::unique_ptr<Buffer<G>> modelInstances;

	// todo: move these elsewhere
	ConcurrentQueue<TaskHandle> mainCalls; // queue with tasks that will be called once on main thread
	ConcurrentQueue<TaskHandle> drawCalls; // queue with tasks that will be called once on draw thread/task
};

namespace rhi
{

template <GraphicsApi G>
uint32_t DetectSuitableGraphicsDevice(Instance<G>& instance, SurfaceHandle<G> surface);

template <GraphicsApi G>
SwapchainConfiguration<G> DetectSuitableSwapchain(Device<G>& device, SurfaceHandle<G> surface);

template <GraphicsApi G>
void CreateQueues(Rhi<G>& rhi);

template <GraphicsApi G>
void CreateWindowDependentObjects(Rhi<G>& rhi);

template <GraphicsApi G>
Window<G> CreateRhiWindow(
	const std::shared_ptr<Device<G>>& device,
	SurfaceHandle<G>&& surface,
	typename Window<G>::ConfigFile&& windowConfig,
	WindowState&& windowState);

template <GraphicsApi G>
std::unique_ptr<Pipeline<G>> CreatePipeline(const std::shared_ptr<Device<G>>& device);

template <GraphicsApi G>
std::shared_ptr<Device<G>> CreateDevice(const std::shared_ptr<Instance<G>>& instance, uint32_t physicalDeviceIndex);

template <GraphicsApi G>
std::shared_ptr<Instance<G>> CreateInstance(std::string_view name);

} // namespace rhi
