#include "preview.h"

#include <core/application.h>
#include <core/profiling.h>

#include <rhi/rhi.h>

#include <filesystem>

namespace client
{

Preview::Preview(gfx::RHI& rhi, rhi::Extent2d extent)
{
	ZoneScopedN("Preview::Preview");

	auto& device = rhi.GetPrimaryDevice();

	// a pipeline of its own: a pipeline's descriptor sets hold one scene's bindings (it loads the window's pipeline cache,
	// and saves it when destroyed, before the window's pipeline does at exit)
	myPipeline = std::make_unique<gfx::Pipeline>(rhi::PipelineCreateDesc<rhi::kGraphicsApi>{
		device.CreateDeviceObjectCreateDesc("Preview Pipeline"),
		(std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / "pipeline.cache").string(),
		gfx::Scene::DescriptorPoolSizes()});
	myViews = std::make_unique<gfx::Views>(device, glm::uvec2(extent.width, extent.height));
	myOutput = std::make_unique<gfx::ImageOutput>(device, extent);
	myScene = std::make_unique<gfx::Scene>(rhi, *myPipeline, *myViews, *myOutput);
	myViews->UpdateBuffers();
}

Preview::~Preview() = default;

} // namespace client
