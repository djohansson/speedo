#pragma once

#include "capi.h"
#include "command.h"
#include "device.h"
#include "instance.h"
#include "pipeline.h"
#include "renderimageset.h"
#include "shaders/shadertypes.h"
#include "types.h"
#include "window.h"

template <GraphicsApi G>
struct Rhi
{
	std::shared_ptr<Instance<G>> instance;
	std::shared_ptr<Device<G>> device;

	std::shared_ptr<Window<G>> mainWindow;
	std::shared_ptr<Pipeline<G>> pipeline;

	UnorderedMap<QueueContextType, CircularContainer<QueueContext<G>>> queueContexts;

	//std::shared_ptr<ResourceContext<G>> resources;

	std::shared_ptr<RenderImageSet<G>> renderImageSet;

	std::unique_ptr<Buffer<G>> materials;
	std::unique_ptr<Buffer<G>[]> objects;

	Future<void> presentFuture;
};
