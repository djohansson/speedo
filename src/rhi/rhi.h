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
#include <string_view>

struct RHIBase
{
	virtual ~RHIBase() = default;
	[[nodiscard]] virtual GraphicsApi GetApi() const = 0;

	// todo: move these elsewhere?
	ConcurrentQueue<TaskHandle> mainCalls; // queue with tasks that will be called once on main thread
	ConcurrentQueue<TaskHandle> drawCalls; // queue with tasks that will be called once on draw thread/task
};

template <GraphicsApi G>
struct RHI : public RHIBase
{
	[[nodiscard]] GraphicsApi GetApi() const final { return G; }

	std::shared_ptr<Instance<G>> instance;
	std::shared_ptr<Device<G>> device;
	std::unique_ptr<Pipeline<G>> pipeline;
	CreateWindowFunc createWindowFunc;

	UnorderedMap<QueueType, QueueTimelineContext<G>> queues;
	UnorderedMap<WindowHandle, Window<G>> windows;

	// temp until we have a proper resource manager
	UnorderedMap<std::string, PipelineLayoutHandle<G>> pipelineLayouts;
	//std::unique_ptr<ResourceContext<G>> resources;

	std::shared_ptr<RenderImageSet<G>> renderImageSet;

	std::unique_ptr<Buffer<G>> materials;
	std::unique_ptr<Buffer<G>> modelInstances;
};

namespace rhi
{

[[nodiscard]] std::unique_ptr<RHIBase> CreateRHI(GraphicsApi api, std::string_view name, CreateWindowFunc createWindowFunc);
template <GraphicsApi G>
[[nodiscard]] std::unique_ptr<RHIBase> CreateRHI(std::string_view name, CreateWindowFunc createWindowFunc);

namespace detail
{


template <GraphicsApi G>
void ConstructWindowDependentObjects(RHI<G>& rhi);

} // namespace detail

} // namespace rhi
