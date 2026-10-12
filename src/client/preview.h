#pragma once

#include <gfx/gpu.h>
#include <gfx/renderoutput.h>
#include <gfx/scene.h>
#include <gfx/views.h>

#include <memory>

namespace client
{

// an off-screen view of a file, for the file browser: a scene of its own (with its own pipeline, views and image output),
// lit by the procedural sky, which the ui shows as an image (see ImGuiLayer::AddTexture). the draw thread records it
// before the window's scene, so that its image is done before the ui samples it. made on the draw thread, outside of
// the graphics queue's lock (the scene takes it), and kept until shutdown: its loads install from draw calls and timeline
// callbacks, which only the shutdown's settling (see WindowedApplication::Shutdown) waits for
class Preview final
{
public:
	Preview(gfx::RHI& rhi, rhi::Extent2d extent);
	~Preview();

	Preview(const Preview&) = delete;
	Preview& operator=(const Preview&) = delete;

	[[nodiscard]] gfx::Scene& GetScene() noexcept { return *myScene; }
	[[nodiscard]] gfx::Views& GetViews() noexcept { return *myViews; }
	[[nodiscard]] gfx::ImageOutput& GetOutput() noexcept { return *myOutput; }

private:
	// destroyed in reverse: the scene first, then what it draws with and into
	std::unique_ptr<gfx::Pipeline> myPipeline;
	std::unique_ptr<gfx::Views> myViews;
	std::unique_ptr<gfx::ImageOutput> myOutput;
	std::unique_ptr<gfx::Scene> myScene;
};

} // namespace client
