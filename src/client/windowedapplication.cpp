#include "windowedapplicationdetail.h"

namespace client
{

std::mutex WindowedApplication::gDrawMutex{};
core::LoadQueue WindowedApplication::gLoads{};
bool WindowedApplication::gShowAbout = false;
bool WindowedApplication::gShowDemoWindow = false;
bool WindowedApplication::gShowFps = false;
bool WindowedApplication::gShowTps = false;

namespace windowedapplication
{

// recreates the swapchain at the current surface size, and everything sized after it. caller holds gDrawMutex.
// returns false if the surface has no area (minimized), in which case nothing is recreated.
bool RecreateWindowDependentObjects(RHI& rhi, platform::Window& window)
{
	ZoneScopedN("RecreateWindowDependentObjects");

	auto& device = rhi.GetPrimaryDevice();
	auto& swapchain = rhi.GetSwapchain(window);

	auto extent = swapchain.QuerySurfaceExtent();
	if (extent.width == 0 || extent.height == 0)
		return false;

	device.WaitIdle();
	swapchain.CreateSwapchain();

	// the window's size in screen coordinates, as its framebuffer's (the swapchain's) over its content scale
	auto& state = window.GetState();
	state.width = static_cast<uint32_t>(static_cast<float>(swapchain.GetDesc().extent.width) / state.xscale);
	state.height = static_cast<uint32_t>(static_cast<float>(swapchain.GetDesc().extent.height) / state.yscale);

	App().GetViews().OnResizeFramebuffer({extent.width, extent.height});
	App().GetScene().Resize();

	return true;
}

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

	std::unique_lock lock(gDrawMutex);
	std::vector<core::TaskHandle> frameTasks;

	auto& rhi = GetRHI();
	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = GetWindow(GetCurrentWindow());
	auto& swapchain = rhi.GetSwapchain(window);
	auto& pipeline = rhi.GetPrimaryDevice().GetPipeline();
	auto& executor = GetExecutor();

	if (window.IsMinimized())
		return false;

	// e.g. a fullscreen switch can leave the swapchain at the previous size without a matching resize event
	if (swapchain.NeedsRecreate() && !RecreateWindowDependentObjects(rhi, window))
		return false;

	// the file browser's preview, once the ui asks for it: made here, outside of the graphics queue's lock (its scene
	// takes it), and published to the ui thread
	if (!myPreview && myPreviewRequested.load(std::memory_order_relaxed))
	{
		constexpr rhi::Extent2d kPreviewExtent{.width = 768, .height = 768};
		myPreview = std::make_unique<Preview>(rhi, kPreviewExtent);
		myPreviewReady.store(true, std::memory_order_release);
	}

	auto [acquireNextImageSemaphore, lastFrameIndex, newFrameIndex, flipSuccess] = swapchain.Flip();

	// one lock at a time: queue types may alias the same context (and mutex), see Device::GetQueue
	auto queueFamilyIndex = [&device](QueueType type) { return device.GetQueue(type).Read()->queueFamilyIndex; };
	bool dedicatedTransfer = queueFamilyIndex(kQueueTypeTransfer) != queueFamilyIndex(kQueueTypeCompute);
	bool dedicatedCompute = queueFamilyIndex(kQueueTypeCompute) != queueFamilyIndex(kQueueTypeGraphics);

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

	if (flipSuccess)
	{
		auto& lastFrame = swapchain.GetFrames()[lastFrameIndex];
		auto& newFrame = swapchain.GetFrames()[newFrameIndex];

		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [lastGraphicsQueue, lastGraphicsSubmits] = graphics->queues.FetchAdd();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();

		frameTasks.emplace_back(
			core::CreateTask([&executor, &queue = graphicsQueue, &semaphore = graphics->semaphore]
			{ queue.SubmitCallbacks(executor, semaphore.GetValue()); }).handle);

		for (auto& fence : graphicsSubmits.fences)
			fence.Wait();

		graphicsSubmits = {};
		graphicsQueue.SwapAndResetPool();
		
		core::TaskHandle drawCall;
		while (rhi.drawCalls.try_dequeue(drawCall))
		{
			ZoneScopedN("WindowedApplication::Draw::drawCall");
			GetExecutor().Call(drawCall, graphics.Get().get());
		}

		// the frame's exposure histogram, and instance and joint buffers, now that the frame's previous use of them is done
		// (see the fences above)
		myScene->Update(*graphics, newFrameIndex);
		// the same for the preview, if it is drawn this frame (updates submit command buffers of their own, see
		// UpdateBufferOnGraphics, ordered before the frame's)
		bool drawPreview = myPreview && myPreviewVisible.load(std::memory_order_relaxed);
		if (drawPreview)
			myPreview->GetScene().Update(*graphics, newFrameIndex);

		// the frame's command buffer: recorded into by the scenes and the ui (which borrow it), and submitted below
		auto cmd = graphicsQueue.GetPool().Begin();

		ZoneScopedN("WindowedApplication::Draw::submit");

		GPU_SCOPE_COLLECT(cmd, graphicsQueue);

		std::vector<core::TaskHandle> graphicsCallbacks;
		auto runInBackground = [this](std::function<void()> job)
		{
			auto [task, future] = core::CreateTask(std::move(job));
			GetExecutor().Submit(std::span(&task, 1));
		};

		// the preview first: its image is sampled by the ui, drawn over the window's frame below
		if (drawPreview)
		{
			myPreview->GetScene().Record(
				cmd,
				Scene::FrameTarget{
					.frameIndex = static_cast<uint16_t>(newFrameIndex),
					.graphicsQueue = &graphicsQueue,
					.graphicsTimeline = graphics->timeline,
					.runInBackground = runInBackground});
			// a read back of it, if asked for (a model's thumbnail)
			myPreview->RecordCapture(cmd, graphicsCallbacks);
		}

		myScene->Record(
			cmd,
			Scene::FrameTarget{
				.frameIndex = static_cast<uint16_t>(newFrameIndex),
				.graphicsQueue = &graphicsQueue,
				.graphicsTimeline = graphics->timeline,
				.prepareUi = [this, &graphicsCallbacks](CommandBufferHandle cmd) { myImGui->PrepareFrame(cmd, graphicsCallbacks); },
				.drawUi = [this](CommandBufferHandle cmd) { myImGui->Draw(cmd); },
				.runInBackground = runInBackground});

		auto presentInfo = swapchain.PreparePresent();

		SemaphoreHandle acquireNextImageSemaphoreHandle = acquireNextImageSemaphore;
		auto graphicsDoneSemaphore = Semaphore(
			SemaphoreCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("graphicsDoneSemaphore{}", newFrameIndex)),
				SemaphoreType::kBinary
			});
		SemaphoreHandle graphicsDoneSemaphoreHandle = graphicsDoneSemaphore;
		// keeps the semaphore alive until the gpu has waited on it. the acquire fence is owned (and waited on) by the
		// swapchain instead, see Swapchain::myAcquireFences
		graphicsCallbacks.emplace_back(
			core::CreateTask(
				[acquireNextImageSemaphore = std::move(acquireNextImageSemaphore)] {}).handle);
		graphicsCallbacks.emplace_back(
			core::CreateTask(
				[&swapchain, presentIds = std::move(presentInfo.presentIds),
				 graphicsDoneSemaphore = std::move(graphicsDoneSemaphore)]
				 {
					for (auto presentId : presentIds)
						swapchain.WaitPresent(presentId);
				 }).handle);

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
			.waitSemaphores = {graphics->semaphore, acquireNextImageSemaphoreHandle},
			// the acquired image may still be in use by the presentation engine until the acquire semaphore signals:
			// nothing may touch it before that (PipelineStage::kNone waited for nothing, which flickered in fullscreen)
			.waitDstStageMasks = {PipelineStage::kAllGraphics, PipelineStage::kAllCommands},
			.waitSemaphoreValues = {lastGraphicsSubmits.maxTimelineValue, 1},
			.signalSemaphores = {graphics->semaphore, graphicsDoneSemaphoreHandle},
			.signalSemaphoreValues = {++graphics->timeline, 1},
			.callbacks = std::move(graphicsCallbacks)},
			std::move(cmd));

		graphicsSubmits |= graphicsQueue.Submit();

		presentInfo.waitSemaphores.emplace_back(graphicsDoneSemaphoreHandle);

		if (dedicatedCompute)
		{
			auto compute = device.GetQueue(kQueueTypeCompute).Write();
			auto& [computeQueue, computeSubmits] = compute->queues.FetchAdd();

			for (auto& fence : computeSubmits.fences)
				fence.Wait();

			computeSubmits = {};
			computeQueue.SwapAndResetPool();

			computeQueue.EnqueuePresent(std::move(presentInfo));
			PresentResult presentResult = PresentResult::kSuccess;
			computeSubmits |= computeQueue.Present(&presentResult);
			swapchain.OnPresentResult(presentResult);
		}
		else
		{
			graphicsQueue.EnqueuePresent(std::move(presentInfo));
			PresentResult presentResult = PresentResult::kSuccess;
			graphicsSubmits |= graphicsQueue.Present(&presentResult);
			swapchain.OnPresentResult(presentResult);
		}

		static uint64_t gLastFrameSubmitCount = Queue::GetSubmitCount();
		static uint64_t gLastFrameSubmitBatchCount = Queue::GetSubmitBatchCount();
		auto submitCount = Queue::GetSubmitCount();
		auto submitBatchCount = Queue::GetSubmitBatchCount();
		gFrameSubmitCount.store(static_cast<uint32_t>(submitCount - gLastFrameSubmitCount), std::memory_order_relaxed);
		gFrameSubmitBatchCount.store(static_cast<uint32_t>(submitBatchCount - gLastFrameSubmitBatchCount), std::memory_order_relaxed);
		gLastFrameSubmitCount = submitCount;
		gLastFrameSubmitBatchCount = submitBatchCount;

		using namespace std::chrono_literals;
		static auto gFpsTime = std::chrono::steady_clock::now();
		static uint32_t gFpsFrameCount = 0;
		gFpsFrameCount++;
		if (auto now = std::chrono::steady_clock::now(); now - gFpsTime >= 500ms)
		{
			gFramesPerSecond.store(
				static_cast<float>(gFpsFrameCount) / std::chrono::duration<float>(now - gFpsTime).count(),
				std::memory_order_relaxed);
			gFpsFrameCount = 0;
			gFpsTime = now;
		}
	}

	GetExecutor().Submit(frameTasks);

	return flipSuccess;
}

WindowedApplication::WindowedApplication(
	std::string_view appName, core::Environment&& env, CreateWindowFunc createWindowFunc)
	: platform::WindowedApplication(appName, std::forward<core::Environment>(env), createWindowFunc)
	, myRHI(std::make_unique<RHI>(RHIInitializationData{
		  .name = appName,
		  .window = GetCurrentWindow(), // the window the platform made: the one we draw in
		  .descriptorPoolSizes = Scene::DescriptorPoolSizes()}))
{
	using namespace windowedapplication;

	auto& rhi = GetRHI();
	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = GetWindow(GetCurrentWindow());
	auto& swapchain = rhi.GetSwapchain(window);
	auto& pipeline = device.GetPipeline();

	myViews = std::make_unique<Views>(device, glm::uvec2(swapchain.GetDesc().extent.width, swapchain.GetDesc().extent.height));

	myOutput = std::make_unique<gfx::SwapchainOutput>(rhi, GetCurrentWindow());
	myScene = std::make_unique<Scene>(rhi, pipeline, *myViews, *myOutput);

	{
		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
		myImGui = std::make_unique<ImGuiLayer>(window, swapchain, rhi, graphicsQueue, graphics->queues.Capacity(), myImGuiIniSettings);
	}
}

WindowedApplication::~WindowedApplication()
{
	// can't tear down the rhi here, since Application::Get() already returns null (see Shutdown())
	ENSUREF(!myRHI, "WindowedApplication::Shutdown() must be called before the application is released");
}

void WindowedApplication::Shutdown()
{
	using namespace windowedapplication;

	ZoneScopedN("WindowedApplication::Shutdown");

	if (!myRHI)
		return;

	auto& rhi = GetRHI();
	auto& device = rhi.GetPrimaryDevice();
	auto& executor = GetExecutor();

	// let in-flight frame tasks (from the last Draw) and loads finish before tearing anything down
	executor.JoinAll();

	// settle: run the draw calls the draw task didn't get to, wait for the gpu, and run the timeline callbacks that are
	// now due, until no draw calls remain. an unrun task is never destroyed, and would leak what it holds past the device:
	// e.g. a load completing during shutdown queues an install, and installing an image queues another draw call from
	// its transition's timeline callback.
	for (bool settled = false; !settled;)
	{
		settled = true;
		{
			auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
			core::TaskHandle drawCall;
			while (rhi.drawCalls.try_dequeue(drawCall))
			{
				executor.Call(drawCall, graphics.Get().get());
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

	myImGui.reset();
	myThumbnails.reset();
	myImagePreview.reset();

	// what holds gpu objects, before the rhi they belong to
	myPreview.reset();
	myScene.reset();
	myOutput.reset();
	myViews.reset();
	myRHI.reset();
}

void WindowedApplication::OnResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace windowedapplication;

	std::unique_lock lock(gDrawMutex);

	ZoneScopedN("WindowedApplication::OnResizeFramebuffer");

	// minimizing reports 0x0: keep the swapchain, and have Draw skip frames until the window is restored
	platform::WindowedApplication::OnResizeFramebuffer(window, width, height);
	auto& platformWindow = GetWindow(window);
	if (platformWindow.IsMinimized())
		return;

	// the swapchain is sized after the surface (which already has this size) rather than width/height
	RecreateWindowDependentObjects(GetRHI(), platformWindow);
}

} // namespace client
