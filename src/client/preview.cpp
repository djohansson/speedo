#include "preview.h"

#include <core/application.h>
#include <core/profiling.h>

#include <rhi/rhi.h>

#include <cstring>
#include <filesystem>

namespace client
{

Preview::Preview(gfx::RHI& rhi, rhi::Extent2d extent)
	: myDevice(rhi.GetPrimaryDevice())
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

void Preview::RequestCapture(CaptureDone done)
{
	std::scoped_lock lock(myCaptureMutex);
	myCapture = std::move(done);
}

void Preview::RecordCapture(CommandBufferHandle<rhi::kGraphicsApi> cmd, std::vector<core::TaskHandle>& callbacks)
{
	std::optional<CaptureDone> done;
	{
		std::scoped_lock lock(myCaptureMutex);
		done.swap(myCapture);
	}
	if (!done)
		return;

	auto& image = myOutput->GetImage();
	auto extent = myOutput->GetExtent();
	auto size = static_cast<size_t>(extent.width) * extent.height * 4;
	auto buffer = std::make_shared<gfx::Buffer>(gfx::BufferCreateDesc{
		myDevice.CreateDeviceObjectCreateDesc("Preview Capture"),
		size,
		rhi::BufferUsage::kTransferDestination,
		rhi::MemoryProperty::kHostVisible});

	// out of the layout the ui samples it in, and back, with the copy's writes made visible to the host
	gfx::CommandEncoder encoder(cmd);
	image.Transition(cmd, rhi::ImageLayout::kTransferSource, rhi::ImageAspect::kColor);
	encoder.CopyImageToBuffer(image, *buffer);
	image.Transition(cmd, rhi::ImageLayout::kShaderReadOnly, rhi::ImageAspect::kColor);
	encoder.Barrier(rhi::PipelineStage::kTransfer, rhi::Access::kTransferWrite, rhi::PipelineStage::kHost, rhi::Access::kHostRead);

	callbacks.emplace_back(
		core::CreateTask(
			[buffer, extent, size, done = std::move(*done)]
			{
				std::vector<uint8_t> rgba(size);
				auto memory = buffer->Map();
				std::memcpy(rgba.data(), memory.data(), size);
				buffer->Unmap();
				done(std::move(rgba), extent.width, extent.height);
			}).handle);
}

} // namespace client
