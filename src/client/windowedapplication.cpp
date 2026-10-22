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

// with SPEEDO_GPU_TIMINGS=1, the frame time of each half second the frame rate is measured over (milliseconds), whose
// median Shutdown prints: unthrottled (SPEEDO_PRESENT_MODE=immediate) the gpu's whole frame, where a backend doesn't
// time every pass (KosmicKrisp, see "GPU timings" in CLAUDE.md)
std::vector<float> gFrameTimes;
const bool gCollectFrameTimes = []
{
	const char* collect = std::getenv("SPEEDO_GPU_TIMINGS");
	return collect != nullptr && std::string_view(collect) == "1";
}();

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

	// the presentation the ui asked for (View > Presentation), applied by recreating the swapchain below
	if (auto request = myPresentationRequest.exchange(0, std::memory_order_relaxed); request != 0)
	{
		auto presentation = UnpackPresentation(request);
		swapchain.Configure(presentation.mode, presentation.imageCount);
	}

	// e.g. a fullscreen switch can leave the swapchain at the previous size without a matching resize event
	if (swapchain.NeedsRecreate() && !RecreateWindowDependentObjects(rhi, window))
		return false;
	auto [minImageCount, maxImageCount] = swapchain.GetImageCountLimits();
	myPresentation.store(
		PackPresentation(Presentation{
			.mode = swapchain.GetPresentationMode(),
			.imageCount = swapchain.GetImageCount(),
			.modes = swapchain.GetPresentationModes(),
			.minImageCount = minImageCount,
			.maxImageCount = maxImageCount}),
		std::memory_order_relaxed);

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

		// the queue's gpu scopes the gpu is done with, for Tracy and the renderers' pass timings (see Renderer::Record)
		if (auto* profiler = graphicsQueue.GetProfiler())
			profiler->Collect();
		graphicsQueue.SwapAndResetPool();
		
		core::TaskHandle drawCall;
		while (rhi.drawCalls.try_dequeue(drawCall))
		{
			ZoneScopedN("WindowedApplication::Draw::drawCall");
			GetExecutor().Call(drawCall, graphics.Get().get());
		}

		// the frame's exposure histogram, and instance and joint buffers, now that the frame's previous use of them is done
		// (see the fences above)
		myScene->Update(*graphics, frameSlot);
		// the same for the preview, if it is drawn this frame (updates submit command buffers of their own, see
		// UpdateBufferOnGraphics, ordered before the frame's)
		bool drawPreview = myPreview && myPreviewVisible.load(std::memory_order_relaxed);
		if (drawPreview)
			myPreview->GetScene().Update(*graphics, frameSlot);

		// the frame's command buffer: recorded into by the scenes and the ui (which borrow it), and submitted below
		auto cmd = graphicsQueue.GetPool().Begin();

		ZoneScopedN("WindowedApplication::Draw::submit");

		std::vector<core::TaskHandle> graphicsCallbacks;

		// the preview first: its image is sampled by the ui, drawn over the window's frame below
		if (drawPreview)
		{
			myPreview->GetScene().Record(
				cmd,
				Scene::FrameTarget{
					.frameIndex = frameSlot,
					.graphicsQueue = &graphicsQueue,
					.graphicsTimeline = graphics->timeline,
					.executor = &GetExecutor()});
			// a read back of it, if asked for (a model's thumbnail)
			myPreview->RecordCapture(cmd, graphicsCallbacks);
		}

		// the ui's frame to draw over the window's: taken now, for its frosted panels' blur, recorded before the ui
		auto uiBlurRects = myImGui->TakeFrame();
		myScene->Record(
			cmd,
			Scene::FrameTarget{
				.frameIndex = frameSlot,
				.outputIndex = static_cast<uint16_t>(newFrameIndex),
				.graphicsQueue = &graphicsQueue,
				.graphicsTimeline = graphics->timeline,
				.prepareUi = [this, &graphicsCallbacks](CommandBufferHandle cmd) { myImGui->PrepareFrame(cmd, graphicsCallbacks); },
				.drawUi = [this](CommandBufferHandle cmd) { myImGui->Draw(cmd); },
				.uiBlurRects = uiBlurRects,
				.executor = &GetExecutor()});

		// a read back of the frame into a bitmap file at path, then done (if given) once written: out of the present layout
		// and back
		auto capture = [&](std::string path, std::atomic_uint8_t* done)
		{
			auto extent = swapchain.GetDesc().extent;
			auto size = static_cast<size_t>(extent.width) * extent.height * 4;
			auto buffer = std::make_shared<gfx::Buffer>(gfx::BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("Frame Capture"), size, BufferUsage::kTransferDestination, MemoryProperty::kHostVisible});
			auto layout = newFrame.GetLayout(0);
			newFrame.Transition(cmd, ImageLayout::kTransferSource, ImageAspect::kColor, 0);
			gfx::CommandEncoder encoder(cmd);
			encoder.CopyImageToBuffer(newFrame.GetImages()[0], extent, *buffer);
			newFrame.Transition(cmd, layout, ImageAspect::kColor, 0);
			encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kHost, Access::kHostRead);
			bool bgra = newFrame.GetDesc().imageFormats[0] == Format::kB8G8R8A8Unorm ||
						newFrame.GetDesc().imageFormats[0] == Format::kB8G8R8A8Srgb;
			graphicsCallbacks.emplace_back(
				core::CreateTask(
					[buffer, extent, size, bgra, path = std::move(path), done]
					{
						// a bitmap file header and info header, then the pixels, top down (a negative height), as bgra
						std::vector<std::byte> file(54 + size);
						auto put = [&file](size_t offset, uint32_t value, size_t bytes)
						{
							for (size_t byteIt = 0; byteIt < bytes; byteIt++)
								file[offset + byteIt] = static_cast<std::byte>((value >> (8 * byteIt)) & 0xFFU);
						};
						file[0] = std::byte{'B'};
						file[1] = std::byte{'M'};
						put(2, static_cast<uint32_t>(file.size()), 4);
						put(10, 54, 4);
						put(14, 40, 4);
						put(18, extent.width, 4);
						put(22, static_cast<uint32_t>(-static_cast<int32_t>(extent.height)), 4);
						put(26, 1, 2);
						put(28, 32, 2);
						auto memory = buffer->Map();
						buffer->Invalidate(0, size);
						std::memcpy(file.data() + 54, memory.data(), size);
						buffer->Unmap();
						if (!bgra)
							for (size_t pixelIt = 54; pixelIt + 3 < file.size(); pixelIt += 4)
								std::swap(file[pixelIt], file[pixelIt + 2]);
						if (!core::file::Write(path, file))
							std::println(stderr, "SPEEDO_CAPTURE: couldn't write {}", path);
						if (done != nullptr)
							*done = 3;
					}).handle);
		};
		// the frame an autoload exit ends on (SPEEDO_CAPTURE, see gCaptureState)
		if (uint8_t requested = 1; gCaptureState.compare_exchange_strong(requested, 2))
			capture(gCapturePath, &gCaptureState);
		// each frame drawing a pose of SPEEDO_CAMERA_PATH for the first time (SPEEDO_CAPTURE_PATH=<directory>:
		// pose-<NNNN>.bmp), to compare a move's frames
		static const char* gCapturePathDirectory = std::getenv("SPEEDO_CAPTURE_PATH");
		if (gCapturePathDirectory != nullptr && *gCapturePathDirectory != '\0')
			if (auto pose = myViews->GetPlacedPose())
				capture((std::filesystem::path(gCapturePathDirectory) / std::format("pose-{:04}.bmp", *pose)).string(), nullptr);

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
		myFrameSlotTimelines[frameSlot] = graphics->timeline;

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

		// the frame rate, and the gpu submits (and their batches) per frame, over the frames of (at least) half a second
		using namespace std::chrono_literals;
		static auto gFpsTime = std::chrono::steady_clock::now();
		static uint32_t gFpsFrameCount = 0;
		static uint64_t gFpsSubmitCount = Queue::GetSubmitCount();
		static uint64_t gFpsSubmitBatchCount = Queue::GetSubmitBatchCount();
		gFpsFrameCount++;
		gDrawnFrameCount.fetch_add(1, std::memory_order_relaxed);
		if (auto now = std::chrono::steady_clock::now(); now - gFpsTime >= 500ms)
		{
			auto submitCount = Queue::GetSubmitCount();
			auto submitBatchCount = Queue::GetSubmitBatchCount();
			auto frames = static_cast<float>(gFpsFrameCount);
			gFramesPerSecond.store(frames / std::chrono::duration<float>(now - gFpsTime).count(), std::memory_order_relaxed);
			if (windowedapplication::gCollectFrameTimes)
				windowedapplication::gFrameTimes.push_back(std::chrono::duration<float, std::milli>(now - gFpsTime).count() / frames);
			gSubmitsPerFrame.store(static_cast<float>(submitCount - gFpsSubmitCount) / frames, std::memory_order_relaxed);
			gSubmitBatchesPerFrame.store(static_cast<float>(submitBatchCount - gFpsSubmitBatchCount) / frames, std::memory_order_relaxed);
			gFpsFrameCount = 0;
			gFpsTime = now;
			gFpsSubmitCount = submitCount;
			gFpsSubmitBatchCount = submitBatchCount;
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
		  .descriptorPoolSizes = Scene::DescriptorPoolSizes(),
		  // ComputeMain's storage views of them (see RenderOutput)
		  .maxSwapchainImageCount = SHADER_TYPES_OUTPUT_IMAGE_COUNT}))
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

	if (gFrameTimes.size() > 2)
	{
		// the first half seconds load and compile
		auto times = std::vector(gFrameTimes.begin() + static_cast<ptrdiff_t>(gFrameTimes.size() / 4), gFrameTimes.end());
		std::ranges::sort(times);
		std::println("frame time: {:.3f} ms (median of {} half seconds)", times[times.size() / 2], times.size());
	}

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
