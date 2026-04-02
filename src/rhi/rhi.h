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

template <GraphicsApi G>
class RHI : public RHIBase
{
public:
	constexpr RHI() noexcept = delete;
	RHI(std::string_view name, CreateWindowFunc createWindowFunc);
	RHI(const RHI&) = delete;
	RHI(RHI&& other) noexcept = delete;

	[[nodiscard]] auto& GetWindows() noexcept { return myWindows; }
	[[nodiscard]] const auto& GetWindows() const noexcept { return myWindows; }
	[[nodiscard]] Window<G>& GetWindow(WindowHandle handle) { return const_cast<Window<G>&>(*myWindows.find(handle)); }
	[[nodiscard]] const Window<G>& GetWindow(WindowHandle handle) const { return *myWindows.find(handle); }
	[[nodiscard]] auto& GetPipelineLayout(const std::string& name) const { return myPipelineLayouts.at(name); }
	[[nodiscard]] auto& GetPipelineLayouts() { return myPipelineLayouts; }
	[[nodiscard]] auto& GetPipeline() { return myPipeline; }
	[[nodiscard]] auto& GetDevice() { return myDevice; }
	[[nodiscard]] auto& GetInstance() { return myInstance; }
	[[nodiscard]] auto& GetQueues() { return myQueues; }
	[[nodiscard]] auto& GetResources() { return myResources; }

private:
	std::shared_ptr<Instance<G>> myInstance;
	std::shared_ptr<Device<G>> myDevice;
	std::unique_ptr<Pipeline<G>> myPipeline;
	CreateWindowFunc myCreateWindowFunc;

	UnorderedMap<QueueType, QueueTimelineContext<G>> myQueues;
	std::flat_set<Window<G>, HandleCompareLess<Window<G>, WindowHandle>> myWindows;

	// temp until we have a proper resource manager
	UnorderedMap<std::string, PipelineLayoutHandle<G>> myPipelineLayouts;

	struct Resources
	{
		// std::vector<std::unique_ptr<Buffer<G>>> buffers;
		// std::vector<std::unique_ptr<Image<G>>> images;
		// std::vector<std::unique_ptr<ImageView<G>>> imageViews;
		// std::vector<std::unique_ptr<Sampler<G>>> samplers;
		std::unique_ptr<Buffer<G>> materials;
		std::unique_ptr<Buffer<G>> modelInstances;
		std::vector<RenderImageSet<G>> renderImageSets;
	} myResources;
};

namespace rhi
{
namespace detail
{

template <GraphicsApi G>
void ConstructWindowDependentObjects(RHI<G>& rhi);

} // namespace detail

} // namespace rhi
