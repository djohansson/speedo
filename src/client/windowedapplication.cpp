#include "framecapture.h"
#include "windowedapplicationdetail.h"

#include <core/runtimings.h>

namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

std::mutex WindowedApplication::gDrawMutex{};
core::LoadQueue WindowedApplication::gLoads{};
// File > About Speedo... (SPEEDO_SHOW_ABOUT=1 at startup, for a capture of it)
bool WindowedApplication::gShowAbout = []
{
	const char* show = std::getenv("SPEEDO_SHOW_ABOUT");
	return show != nullptr && std::string_view(show) == "1";
}();
bool WindowedApplication::gShowFps = true; // on by default, the user's call (2026-11-18)

namespace windowedapplication
{

// SPEEDO_UI=0: the ui isn't drawn over the window's frames (nor its panels' blur), for captures of the scene alone; it
// still runs (the autoloads, the camera's placement, the capture itself), and its textures are still uploaded and
// released (PrepareFrame)
const bool gDrawUi = []
{
	const char* setting = std::getenv("SPEEDO_UI");
	return setting == nullptr || std::string_view(setting) != "0";
}();

} // namespace windowedapplication

void WindowedApplication::OnInputStateChanged(const core::InputState& input)
{
	using namespace windowedapplication;
	
	ZoneScopedN("WindowedApplication::OnInputStateChanged");

	auto& imguiIO = ImGui::GetIO();

	if (imguiIO.WantSaveIniSettings)
	{
		size_t iniStringSize;
		const char* iniString = ImGui::SaveIniSettingsToMemory(&iniStringSize);
		myImGuiIniSettings.assign(iniString, iniStringSize);
		imguiIO.WantSaveIniSettings = false;
	}

	// the next frame uploads the views it moved (see Views::Upload)
	if (!imguiIO.WantCaptureMouse && !imguiIO.WantCaptureKeyboard)
		myViews->OnInputStateChanged(input);
}

bool WindowedApplication::Draw()
{
	using namespace windowedapplication;

	FrameMark;
	ZoneScopedN("WindowedApplication::Draw");

	rhi::CrashHeartbeat();

	std::unique_lock lock(gDrawMutex);
	std::vector<core::TaskHandle> frameTasks;

	auto& rhi = GetRHI();
	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = GetWindow(GetCurrentWindow());
	// none headless: the frames are drawn into myImageOutput, and nothing is acquired or presented
	auto* swapchain = myImageOutput == nullptr ? &rhi.GetSwapchain(window) : nullptr;
	auto& pipeline = rhi.GetPrimaryDevice().GetPipeline();
	auto& executor = GetExecutor();

	rhi::CrashFrame(gDrawnFrameCount.load(std::memory_order_relaxed));

	// the window, and what the scenes make, before the frame (see InternalPrepare): none drawn while the window can't be
	if (!InternalPrepare(swapchain))
		return false;

	// the swapchain's next image (headless, the image output's one, always ready)
	auto flip = swapchain != nullptr ? std::optional(swapchain->Flip()) : std::nullopt;
	bool frameReady = !flip || flip->success;
	uint32_t outputIndex = flip ? flip->newFrameIndex : 0;

	// the timeline callbacks of the transfer queues (e.g. what the loaders' uploads release), unless the transfer queue
	// type aliases the graphics queues, whose callbacks are run below
	if (&device.GetQueue(kQueueTypeTransfer) != &device.GetQueue(kQueueTypeGraphics))
	{
		auto transfer = device.GetQueue(kQueueTypeTransfer).Read();
		for (const auto& [queue, submits] : transfer->queues)
			frameTasks.emplace_back(
				core::CreateTask([&executor, &queue = queue, &semaphore = transfer->semaphore]
				{ queue.SubmitCallbacks(executor, semaphore.GetValue()); }).handle);
	}

	if (frameReady)
	{
		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [lastGraphicsQueue, lastGraphicsSubmits] = graphics->queues.FetchAdd();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();

		frameTasks.emplace_back(
			core::CreateTask([&executor, &queue = graphicsQueue, &semaphore = graphics->semaphore]
			{ queue.SubmitCallbacks(executor, semaphore.GetValue()); }).handle);

		for (auto& fence : graphicsSubmits.fences)
			fence.Wait();

		graphicsSubmits = {};

		// the frame's slot of the per frame buffers (the views', the draw records, the indirect lighting's parameters,
		// ...), the frames' in turn, and the last frame in it done: the fences above are the queue ring's, which needn't be
		// as long as the slots (a queue per frame). the slot isn't the swapchain image (newFrameIndex), which is only
		// for what is the image's (ComputeMain's storage view of it, the ui's render target, the capture): fifo hands
		// the images out in turn, mailbox and immediate needn't (an image a newer frame replaced comes back at once), and
		// buffers kept by the image were written over while a frame still on the gpu read them (Sponza's indirect
		// lighting stayed black on Windows), or a frame waited for the one just drawn
		auto frameSlot = static_cast<uint16_t>(myFrameSlot);
		myFrameSlot = (myFrameSlot + 1) % SHADER_TYPES_FRAME_COUNT;
		if (auto timeline = myFrameSlotTimelines[frameSlot]; timeline > 0)
			graphics->semaphore.Wait(timeline);
		// and what the slot's last frame presented with, once the present is done with it
		InternalRetirePresents(frameSlot);

		// the queue's gpu scopes the gpu is done with, for Tracy and the renderers' pass timings (see Renderer::Record)
		if (auto* profiler = graphicsQueue.GetProfiler())
			profiler->Collect();
		graphicsQueue.SwapAndResetPool();
		
		DrawCall drawCall;
		while (rhi.drawCalls.try_dequeue(drawCall))
		{
			ZoneScopedN("WindowedApplication::Draw::drawCall");
			drawCall(*graphics.Get());
		}

		FrameContext frame{
			.graphics = *graphics.Get(),
			.graphicsQueue = graphicsQueue,
			.swapchain = swapchain,
			.frameSlot = frameSlot,
			.outputIndex = static_cast<uint16_t>(outputIndex),
			.drawPreview = myPreview && myPreviewVisible.load(std::memory_order_relaxed),
			.waitTimelineValue = lastGraphicsSubmits.maxTimelineValue,
			.acquireSemaphore = flip ? std::move(flip->acquireNextImageSemaphore) : Semaphore{}};
		InternalUpdate(frame);

		// the frame's command buffer: recorded into by the scenes and the ui (which borrow it), and submitted below
		auto cmd = graphicsQueue.GetPool().Begin();

		ZoneScopedN("WindowedApplication::Draw::submit");

		InternalRecord(cmd, frame);

		InternalSubmit(std::move(cmd), frame);

		// the frame counted: the frame rate, the submits per frame, the frame times (see FrameStats)
		myFrameStats.Count(myScene && myScene->IsReady() && myScene->GetRenderer().IsLightingWarmingUp());
	}

	GetExecutor().Submit(frameTasks);

	return frameReady;
}

void WindowedApplication::InternalSubmit(gfx::CommandBuffer&& cmd, FrameContext& frame)
{
	using namespace windowedapplication;

	ZoneScopedN("WindowedApplication::InternalSubmit");

	auto& device = GetRHI().GetPrimaryDevice();
	auto& graphics = frame.graphics;
	auto& graphicsQueue = frame.graphicsQueue;
	auto& graphicsSubmits = graphics.queues.Get().second; // the frame's queue's, which the present's fences join
	auto* swapchain = frame.swapchain;
	auto frameSlot = frame.frameSlot;
	auto outputIndex = frame.outputIndex;

	QueueDeviceSyncInfo frameSync{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {PipelineStage::kAllGraphics},
		.waitSemaphoreValues = {frame.waitTimelineValue},
		.signalSemaphores = {graphics.semaphore},
		.signalSemaphoreValues = {++graphics.timeline}};

	// what the present needs (none headless): the acquired image, and a semaphore the frame signals for the present
	auto presentInfo = swapchain != nullptr ? std::optional(swapchain->PreparePresent()) : std::nullopt;
	SemaphoreHandle graphicsDoneSemaphoreHandle{};
	if (swapchain != nullptr)
	{
		SemaphoreHandle acquireNextImageSemaphoreHandle = frame.acquireSemaphore;
		auto graphicsDoneSemaphore = Semaphore(
			SemaphoreCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("graphicsDoneSemaphore{}", outputIndex)),
				SemaphoreType::kBinary
			});
		graphicsDoneSemaphoreHandle = graphicsDoneSemaphore;
		// keeps the acquire semaphore alive until the gpu has waited on it. the acquire fence is owned (and waited on) by
		// the swapchain instead, see Swapchain::myAcquireFences
		frame.callbacks.emplace_back(
			core::CreateTask(
				[acquireNextImageSemaphore = std::move(frame.acquireSemaphore)] {}).handle);
		// and the present's until the present is done with it: kept by the frame's slot (see InternalRetirePresents).
		// a timeline callback waited for each present (vkWaitForPresentKHR), blocking an executor thread for a refresh
		// or more under fifo; it never did, since the swapchain's present ids were never on (their extension was looked
		// for among the instance's), so the semaphores went with the frame's gpu work
		myPresentSemaphores[frameSlot].push_back(std::move(graphicsDoneSemaphore));

		// the acquired image may still be in use by the presentation engine until the acquire semaphore signals:
		// nothing may touch it before that (PipelineStage::kNone waited for nothing, which flickered in fullscreen)
		frameSync.waitSemaphores.push_back(acquireNextImageSemaphoreHandle);
		frameSync.waitDstStageMasks.push_back(PipelineStage::kAllCommands);
		frameSync.waitSemaphoreValues.push_back(1);
		frameSync.signalSemaphores.push_back(graphicsDoneSemaphoreHandle);
		frameSync.signalSemaphoreValues.push_back(1);
	}
	frameSync.callbacks = std::move(frame.callbacks);
	// SPEEDO_CRASH_TEST=hang: from the 100th frame drawn, the frame waits for a semaphore nothing signals, so that the
	// next wait for it reports a hang (rhi::CrashEvent's report, see rhi/vulkan/crashreport.h), to check the reports end
	// to end (with SPEEDO_GPU_HANG_TIMEOUT=<seconds> to see it sooner)
	static const bool kCrashTestHang = []
	{
		const char* test = std::getenv("SPEEDO_CRASH_TEST");
		return test != nullptr && std::string_view(test) == "hang";
	}();
	constexpr uint64_t kCrashTestFrame = 100;
	if (kCrashTestHang && gDrawnFrameCount.load(std::memory_order_relaxed) >= kCrashTestFrame)
	{
		static Semaphore gNeverSignaled(
			SemaphoreCreateDesc{device.CreateDeviceObjectCreateDesc("SPEEDO_CRASH_TEST"), SemaphoreType::kTimeline});
		static const bool kOnce = []
		{
			rhi::CrashEvent("SPEEDO_CRASH_TEST=hang: the frame waits for a semaphore nothing signals");
			return true;
		}();
		frameSync.waitSemaphores.push_back(gNeverSignaled);
		frameSync.waitDstStageMasks.push_back(PipelineStage::kAllCommands);
		frameSync.waitSemaphoreValues.push_back(1);
	}
	graphicsQueue.EnqueueSubmit(std::move(frameSync), std::move(cmd));
	myFrameSlotTimelines[frameSlot] = graphics.timeline;

	graphicsSubmits |= graphicsQueue.Submit();

	// from the queue the swapchain says (Swapchain::GetPresentQueueType: a dedicated compute queue where its family can
	// present, its images shared by both families, else the graphics queue)
	if (swapchain != nullptr)
	{
		presentInfo->waitSemaphores.emplace_back(graphicsDoneSemaphoreHandle);

		PresentResult presentResult = PresentResult::kSuccess;
		if (swapchain->GetPresentQueueType() != kQueueTypeGraphics)
		{
			// one lock at a time: the compute queue's is another context than the graphics queue's here (another
			// family), so holding both doesn't deadlock (see Device::GetQueue)
			auto present = device.GetQueue(swapchain->GetPresentQueueType()).Write();
			auto& [presentQueue, presentSubmits] = present->queues.Get();
			presentQueue.EnqueuePresent(std::move(*presentInfo));
			auto presented = presentQueue.Present(&presentResult);
			std::ranges::move(presented.fences, std::back_inserter(myPresentFences[frameSlot]));
		}
		else
		{
			graphicsQueue.EnqueuePresent(std::move(*presentInfo));
			auto presented = graphicsQueue.Present(&presentResult);
			std::ranges::move(presented.fences, std::back_inserter(myPresentFences[frameSlot]));
		}
		swapchain->OnPresentResult(presentResult);
	}
}

bool WindowedApplication::InternalPrepare(gfx::Swapchain* swapchain)
{
	ZoneScopedN("WindowedApplication::InternalPrepare");

	// the window's changes, its presentation and its size, applied before the frame (see InternalPrepareWindow): none
	// drawn while it can't be
	if (!InternalPrepareWindow(swapchain))
		return false;

	// what the scene makes once its shaders have compiled in the background (see Scene::Prepare: the first launch's take
	// tens of seconds, during which the frames are the ui alone), outside of the graphics queue's lock (it takes it)
	myScene->Prepare();

	// the file browser's preview, once the ui asks for it: made here, outside of the graphics queue's lock (its scene
	// takes it), and published to the ui thread. once the window's scene is ready: its shaders, the same, are then
	// cached, rather than compiled twice at once
	if (!myPreview && myPreviewRequested.load(std::memory_order_relaxed) && myScene->IsReady())
	{
		constexpr rhi::Extent2d kPreviewExtent{.width = 768, .height = 768};
		myPreview = std::make_unique<Preview>(GetRHI(), kPreviewExtent);
		// its shaders reloaded with the window's (the same layouts, compiled once: see Scene::FollowShaders)
		myPreview->GetScene().FollowShaders(*myScene);
		myPreviewReady.store(true, std::memory_order_release);
	}
	if (myPreview)
		myPreview->GetScene().Prepare();

	// and the ui's, once the window's scene reloaded them (see Scene::AddReloadedLayout), between frames
	if (auto reloaded = myScene->GetReloadedShaders(); reloaded && reloaded->generation > myImGuiShaderGeneration)
	{
		auto layout = std::ranges::find(reloaded->layouts, std::string_view("ImGui"), [](const auto& entry) { return std::string_view(entry.first); });
		if (layout == reloaded->layouts.end() || myImGuiRenderer->ReplaceShaders(layout->second))
			myImGuiShaderGeneration = reloaded->generation;
	}

	return true;
}

void WindowedApplication::InternalUpdate(FrameContext& frame)
{
	ZoneScopedN("WindowedApplication::InternalUpdate");

	// the frame's exposure histogram, and instance and joint buffers, now that the frame's previous use of them is done
	// (see Draw's fences)
	myScene->Update(frame.graphics, frame.frameSlot);
	// the same for the preview, if it is drawn this frame (updates submit command buffers of their own, see
	// UpdateBufferOnGraphics, ordered before the frame's)
	if (frame.drawPreview)
		myPreview->GetScene().Update(frame.graphics, frame.frameSlot);
}

void WindowedApplication::InternalRecord(CommandBufferHandle cmd, FrameContext& frame)
{
	using namespace windowedapplication;

	ZoneScopedN("WindowedApplication::InternalRecord");

	// the preview first: its image is sampled by the ui, drawn over the window's frame below
	if (frame.drawPreview)
	{
		myPreview->GetScene().Record(
			cmd,
			Scene::FrameTarget{
				.frameIndex = frame.frameSlot,
				.graphicsQueue = &frame.graphicsQueue,
				.graphicsTimeline = frame.graphics.timeline,
				.executor = &GetExecutor()});
		// a read back of it, if asked for (a model's thumbnail)
		myPreview->RecordCapture(cmd, frame.callbacks);
	}

	// the ui's frame to draw over the window's: taken now, for its frosted panels' blur, recorded before the ui (the scene
	// calls the ui's callables during its Record, see Scene::FrameTarget)
	auto uiFrame = myImGui->TakeFrame();
	myUiBlurRects.clear();
	if (gDrawUi)
		for (const auto& panel : uiFrame.panels)
		{
			UiBlurRect rect{};
			std::ranges::copy(panel.rect, std::begin(rect.rect));
			rect.radius = panel.radius;
			rect.opacity = panel.opacity;
			myUiBlurRects.push_back(rect);
		}
	const auto frameSlot = frame.frameSlot;
	myScene->Record(
		cmd,
		Scene::FrameTarget{
			.frameIndex = frameSlot,
			.outputIndex = frame.outputIndex,
			.graphicsQueue = &frame.graphicsQueue,
			.graphicsTimeline = frame.graphics.timeline,
			.prepareUi = [this, &uiFrame, &frame, frameSlot](CommandBufferHandle cmd)
			{ myImGuiRenderer->PrepareFrame(cmd, uiFrame.drawData, uiFrame.sequence, frameSlot, frame.callbacks); },
			.drawUi = [this, &uiFrame, frameSlot](CommandBufferHandle cmd, gfx::IRenderTarget& target)
			{
				if (gDrawUi)
					myImGuiRenderer->Draw(cmd, uiFrame.drawData, frameSlot, target);
			},
			.uiBlurRects = myUiBlurRects,
			.executor = &GetExecutor()});

	// the captures this frame asks for (SPEEDO_CAPTURE, SPEEDO_CAPTURE_PATH), after the scene and the ui (see FrameCapture)
	RecordFrameCaptures(
		cmd,
		FrameCaptureTarget{
			.device = GetRHI().GetPrimaryDevice(),
			.swapchain = frame.swapchain,
			.imageOutput = myImageOutput,
			.extent = myOutput->GetExtent(),
			.outputIndex = frame.outputIndex,
			.placedPose = myViews->GetPlacedPose()},
		frame.callbacks);
}

WindowedApplication::WindowedApplication(
	std::string_view appName, core::Environment&& env)
	: platform::WindowedApplication(appName, std::forward<core::Environment>(env))
	, myRHI((rhi::SetCrashReportDirectory(
				 (std::get<std::filesystem::path>(GetEnv().variables["UserProfilePath"]) / "crashes").string()),
			 std::make_unique<RHI>(RHIInitializationData{
		  .name = appName,
		  // the window the platform made: the one we draw in. none headless (PlatformIsHeadless): no surface or swapchain,
		  // the frames are drawn into an image (myImageOutput)
		  .window = PlatformIsHeadless() ? kInvalidWindowHandle : GetCurrentWindow(),
		  .descriptorPoolSizes = Scene::DescriptorPoolSizes(),
		  // ComputeMain's storage views of them (see RenderOutput)
		  .maxSwapchainImageCount = SHADER_TYPES_OUTPUT_IMAGE_COUNT,
		  // (see client::Create)
		  .driverCachePath = std::get<std::filesystem::path>(GetEnv().variables["DriverCachePath"]).string()})))
{
	using namespace windowedapplication;

	core::SetRunTimingFrames(&gDrawnFrameCount);
	core::RunTiming("the rhi made");

	auto& rhi = GetRHI();
	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = GetWindow(GetCurrentWindow());
	auto& pipeline = device.GetPipeline();

	// the frames' size: the swapchain's, or headless the window's framebuffer's (the null platform's window, of
	// SPEEDO_WINDOW_SIZE's size)
	rhi::Extent2d extent{};
	if (PlatformIsHeadless())
	{
		const auto& state = window.GetState();
		extent = {
			.width = static_cast<uint32_t>(static_cast<float>(state.width) * state.xscale),
			.height = static_cast<uint32_t>(static_cast<float>(state.height) * state.yscale)};
		myFramebufferSize.store((static_cast<uint64_t>(extent.width) << 32U) | extent.height, std::memory_order_relaxed);
	}
	else
	{
		extent = rhi.GetSwapchain(window).GetDesc().extent;
	}

	myViews = std::make_unique<Views>(device, glm::uvec2(extent.width, extent.height));

	// the ui: imgui's context and frames (platform's), and its renderer (gfx's), whose shaders compile before the scene's
	// start in the background (slang is used by one thread at a time, see gfx::ShaderLoader)
	myImGui = std::make_unique<platform::ImGuiLayer>(window, myImGuiIniSettings);
	myImGuiRenderer = std::make_unique<gfx::ImGuiRenderer>(device);

	if (PlatformIsHeadless())
	{
		auto output = std::make_unique<gfx::ImageOutput>(device, extent, true);
		myImageOutput = output.get();
		myOutput = std::move(output);
	}
	else
	{
		myOutput = std::make_unique<gfx::SwapchainOutput>(rhi, GetCurrentWindow());
	}
	myScene = std::make_unique<Scene>(rhi, pipeline, *myViews, *myOutput);
	// its shaders reloaded when their sources change (a checkout's: see Scene::WatchShaders), unless SPEEDO_SHADER_RELOAD=0
	const char* reload = std::getenv("SPEEDO_SHADER_RELOAD");
	myScene->WatchShaders(reload == nullptr || std::string_view(reload) != "0");
	// and the graphics pipelines its passes ask for kept in the profile, asked for ahead in the next run's start
	myScene->KeepGraphicsPipelines(std::get<std::filesystem::path>(GetEnv().variables["UserProfilePath"]) / "graphics-pipelines.txt");
	// and the ui's with them (one compile at a time, see gfx::ShaderLoader), which InternalPrepare gives the ImGui renderer
	myScene->AddReloadedLayout("ImGui", gfx::ImGuiShaderLayout(std::get<std::filesystem::path>(GetEnv().variables["ShaderPath"])));
	core::RunTiming("the views, the ui and the scene made");
}

WindowedApplication::~WindowedApplication()
{
	// can't tear down the rhi here, since Application::Get() already returns null (see Shutdown())
	ENSUREF(!myRHI, "WindowedApplication::Shutdown() must be called before the application is released");
}

void WindowedApplication::InternalRetirePresents(uint32_t frameSlot)
{
	// a present fence signals once the present is done with its semaphores (VK_EXT_swapchain_maintenance1); without
	// them, a slot's frame was presented SHADER_TYPES_FRAME_COUNT frames ago, which the next acquires came after
	constexpr uint64_t kPresentTimeout = 1'000'000'000; // ns: a present the window never shows doesn't hang the frame
	for (const auto& fence : myPresentFences[frameSlot])
		fence.Wait(kPresentTimeout);
	myPresentFences[frameSlot].clear();
	myPresentSemaphores[frameSlot].clear();
}

void WindowedApplication::InternalRetirePresents(gfx::Swapchain& swapchain)
{
	// every frame's present shown, before the window changes (see Draw) or the swapchain goes at shutdown: with fifo,
	// frames queued for the coming vertical blanks when exclusive fullscreen changed the display mode were never shown,
	// and NVIDIA's driver then waited for them in vkCreateSwapchainKHR or vkDestroySwapchainKHR, forever
	constexpr uint64_t kPresentedTimeout = 1'000'000'000; // ns
	bool presented = swapchain.WaitPresented(kPresentedTimeout);
	if (!presented && swapchain.HasPresented())
		std::this_thread::sleep_for(std::chrono::milliseconds(50)); // a few refreshes, where present waits aren't had
	for (uint32_t slot = 0; slot < SHADER_TYPES_FRAME_COUNT; slot++)
		InternalRetirePresents(slot);
}

void WindowedApplication::Shutdown()
{
	using namespace windowedapplication;

	ZoneScopedN("WindowedApplication::Shutdown");

	if (!myRHI)
		return;

	rhi::CrashEvent("shutdown");

	myFrameStats.PrintFrameTimes();
	// what auto exposure settled on, to capture the same view at it (SPEEDO_AUTO_EXPOSURE=0 SPEEDO_EXPOSURE=<stops>):
	// measured as it adapts, a capture's change mixes the exposure's with the scene's
	if (myScene && myScene->GetSettings().AutoExposure())
	{
		auto exposure = myScene->GetViewExposure();
		std::println(
			"auto exposure: {:+.2f} EV (f/{:.3g}, {} s, ISO {:.0f}, EV100 {:.2f})", static_cast<double>(exposure.Stops()), static_cast<double>(exposure.aperture),
			exposure.shutter < 1.0F ? std::format("1/{:.0f}", 1.0 / static_cast<double>(exposure.shutter)) : std::format("{:.3g}", static_cast<double>(exposure.shutter)),
			static_cast<double>(exposure.iso), static_cast<double>(exposure.Ev100()));
	}
	// and where the model's animation got to, to capture what moves where it was, paused (SPEEDO_ANIMATION_TIME=<seconds>;
	// with SPEEDO_ANIMATION_FRAME_SECONDS the same from run to run)
	if (myScene)
	{
		auto animation = myScene->GetAnimation().Read();
		const auto& state = animation.Get();
		if (state.selected && *state.selected < state.names.size())
			std::println("animation: '{}' at {:.6f} s{}", state.names[*state.selected], state.time, state.playing ? "" : ", paused");
	}

	auto& rhi = GetRHI();
	auto& device = rhi.GetPrimaryDevice();
	auto& executor = GetExecutor();

	// let in-flight frame tasks (from the last Draw) and loads finish before tearing anything down
	executor.JoinAll();
	core::RunTiming("shutdown: the tasks joined");

	// settle: run the draw calls the draw task didn't get to, wait for the gpu, and run the timeline callbacks that are
	// now due, until no draw calls remain. an unrun task is never destroyed, and would leak what it holds past the device:
	// e.g. a load completing during shutdown queues an install, and installing an image queues another draw call from
	// its transition's timeline callback.
	for (bool settled = false; !settled;)
	{
		settled = true;
		{
			auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
			DrawCall drawCall;
			while (rhi.drawCalls.try_dequeue(drawCall))
			{
				drawCall(*graphics.Get());
				settled = false;
			}
		}
		executor.JoinAll();

		device.WaitIdle();

		// all gpu work has completed, so every pending timeline callback is due (they own per-frame fences/semaphores etc).
		// queue locks are released before joining, since callbacks may access the queues themselves.
		// each distinct queue context once, locking one at a time: queue types may alias the same context (and mutex).
		{
			constexpr auto kAllTimelineValues = std::numeric_limits<uint64_t>::max();

			std::vector<QueueTimelineContext*> visited;
			for (auto type : {kQueueTypeGraphics, kQueueTypeCompute, kQueueTypeTransfer})
			{
				auto* context = &device.GetQueue(type);
				if (std::ranges::contains(visited, context))
					continue;
				visited.emplace_back(context);

				auto queues = context->Write();
				for (auto& [queue, submits] : queues->queues)
					queue.SubmitCallbacks(executor, kAllTimelineValues);
			}
		}
		executor.JoinAll();

		if (rhi.drawCalls.size_approx() != 0)
			settled = false;
	}
	core::RunTiming("shutdown: the gpu idle, the callbacks run");

	// what the last frames presented with, once shown (the swapchain goes with the rhi). nothing headless
	if (myImageOutput == nullptr)
		InternalRetirePresents(rhi.GetSwapchain(GetWindow(GetCurrentWindow())));

	// the renderer before the imgui context it draws (its textures are imgui's)
	myImGuiRenderer.reset();
	myImGui.reset();
	myThumbnails.reset();
	myImagePreview.reset();
	myAboutIcon.reset();
	core::RunTiming("shutdown: the ui released");

	// what holds gpu objects, before the rhi they belong to
	myPreview.reset();
	myScene.reset();
	core::RunTiming("shutdown: the scene released");
	myOutput.reset();
	myViews.reset();
	myRHI.reset();
	core::RunTiming("shutdown: the rhi released");
}

} // namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
