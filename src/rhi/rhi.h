#pragma once

#include "capi.h"
#include "command.h"
#include "device.h"
#include "instance.h"
#include "pipeline.h"
#include "queue.h"
#include "renderimageset.h"
#include "semaphore.h"
#include "shaders/shadertypes.h"
#include "types.h"
#include "window.h"
#include "utils.h"

#include <core/utils.h>

template <GraphicsApi G>
struct Rhi
{
	std::shared_ptr<Instance<G>> instance;
	std::shared_ptr<Device<G>> device;

	std::unique_ptr<Window<G>> mainWindow;
	std::unique_ptr<Pipeline<G>> pipeline;

	UnorderedMap<QueueContextType, std::vector<QueueContext<G>>> queueContexts;

	//std::unique_ptr<ResourceContext<G>> resources;

	std::shared_ptr<RenderImageSet<G>> renderImageSet;

	std::unique_ptr<Buffer<G>> materials;
	std::unique_ptr<Buffer<G>[]> objects;

	ConcurrentQueue<TaskHandle> mainCalls;

	std::unique_ptr<Semaphore<G>> timelineSemaphore;
};
