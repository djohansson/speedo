#include <gfx/capi.h>
#include <gfx/windowedapplication.h>
#include <gfx/meshimport.h>
#include <gfx/model.h>
#include <gfx/shaderloader.h>
#include <gfx/texture.h>

#include <core/task.h>
#include <rhi/capi.h>
#include <rhi/renderimageset.h>
#include <gfx/shaders/capi.h>

#include <uuid.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <GLFW/glfw3.h>

#include <gfx/gpu.h>
#include <gfx/imgui_extra.h>
#include <gfx/ziparchive.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <limits>
#include <span>
#include <array>
#include <memory>

//#include <imnodes.h>

namespace gfx
{

using namespace rhi;

std::mutex WindowedApplication::gDrawMutex{};
core::LoadQueue WindowedApplication::gLoads{};
bool WindowedApplication::gShowAbout = false;
bool WindowedApplication::gShowDemoWindow = false;
bool WindowedApplication::gShowFps = false;
bool WindowedApplication::gShowTps = false;

static std::optional<WindowHandle> gCurrentWindow{};

namespace windowedapplication
{

[[nodiscard]] static WindowedApplication& App()
{
	auto app = std::static_pointer_cast<WindowedApplication>(core::Application::Get());
	ENSURE(app);
	return *app;
}

// imgui draw data is handed from PrepareDraw (tick task) to IMGUIDraw (draw task) through a triple buffer:
// PrepareDraw snapshots into its write frame and publishes it as the pending one, the draw thread takes the pending
// frame as its read frame. each frame's snapshot owns its draw lists, so neither thread touches the other's.
struct IMGUIFrame
{
	imgui_extra::ImDrawDataSnapshot snapshot;
	ImDrawData drawData;
	uint64_t sequence = 0; // PrepareDraw count when published
};
static std::array<IMGUIFrame, 3> gIMGUIFrames;
static constexpr uint8_t kIMGUIFrameFresh = 0x80; // set on gIMGUIPendingFrame when published but not yet taken
static uint8_t gIMGUIWriteFrame = 0; // PrepareDraw only
static uint8_t gIMGUIReadFrame = 1; // draw thread only
static std::atomic_uint8_t gIMGUIPendingFrame = 2;
static uint64_t gIMGUIFrameSequence = 0; // PrepareDraw only

// imgui's renderer, see ImGuiRenderer for which thread calls what
static std::unique_ptr<ImGuiRenderer> gIMGUIRenderer;

// gpu submits (and their batches) between the last two presented frames, from any thread. written by Draw
static std::atomic_uint32_t gFrameSubmitCount;
static std::atomic_uint32_t gFrameSubmitBatchCount;
// presented frames per second, over (at least) the last half second. written by Draw
static std::atomic<float> gFramesPerSecond;
static std::array<uuids::uuid, 3> gRenderImageSetUuids;
static std::shared_ptr<Model> gModel; // the loaded model, see InstallModel. only the draw thread uses it
static uuids::uuid gLoadedImageUuid; // the loaded image and its view, see InstallImage. nil until one is loaded
static uuids::uuid gLoadedImageViewUuid;
static uuids::uuid gBlackTextureUuid;
static uuids::uuid gBlackTextureViewUuid;
static uuids::uuid gSamplersUuid;
static uuids::uuid gMaterialsUuid;
static uuids::uuid gModelInstancesUuid;

// takes the latest imgui frame published by PrepareDraw and records the texture uploads it depends on into `cmd`
// (outside of a render pass). textures that are no longer drawn are destroyed from a task added to `callbacks`, which
// must run once the gpu has completed the submission of `cmd`. call on the draw thread, before IMGUIDraw.
static void IMGUIPrepareFrame(CommandBufferHandle cmd, std::vector<core::TaskHandle>& callbacks)
{
	ZoneScopedN("WindowedApplication::IMGUIPrepareFrame");

	// take the frame before the texture uploads: a frame is published after the uploads it depends on were queued.
	// keep drawing the previous frame if no new one was published.
	if ((gIMGUIPendingFrame.load(std::memory_order_relaxed) & kIMGUIFrameFresh) != 0)
		gIMGUIReadFrame = gIMGUIPendingFrame.exchange(gIMGUIReadFrame, std::memory_order_acq_rel) & ~kIMGUIFrameFresh;

	gIMGUIRenderer->PrepareFrame(cmd, gIMGUIFrames[gIMGUIReadFrame].sequence, callbacks);
}

static void IMGUIDraw(CommandBufferHandle cmd)
{
	ZoneScopedN("WindowedApplication::IMGUIDraw");

	gIMGUIRenderer->Render(gIMGUIFrames[gIMGUIReadFrame].drawData, cmd);
}

static void IMGUIInit(
	Window& window,
	RHI& rhi,
	Queue& graphicsQueue,
	uint32_t graphicsQueueCount,
	std::string_view imguiIniSettings)
{
	ZoneScopedN("WindowedApplication::IMGUIInit");

	using namespace ImGui;
	using namespace windowedapplication;

	IMGUI_CHECKVERSION();
	CreateContext();
	auto& imguiIO = GetIO();
	imguiIO.IniFilename = nullptr;

	//imguiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	//imguiIO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	//imguiIO.FontGlobalScale = 1.0f;
	//imguiIO.FontAllowUserScaling = true;

	LoadIniSettingsFromMemory(imguiIniSettings.data(), imguiIniSettings.size());

#if defined(__OSX__)
	float dpiScaleX = 1.0F;
	float dpiScaleY = 1.0F;
#else
	float dpiScaleX = window.GetDesc().contentScale.x;
	float dpiScaleY = window.GetDesc().contentScale.y;
#endif

	imguiIO.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

	GetStyle().ScaleAllSizes(std::max(dpiScaleX, dpiScaleY));

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.RasterizerDensity = std::max(window.GetDesc().contentScale.x, window.GetDesc().contentScale.y);
	config.PixelSnapH = false;

	imguiIO.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	std::filesystem::path fontPath(std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]));
	fontPath /= "fonts";
	fontPath /= "foo";

	constexpr std::array<const char*, 6> kFonts{{
		"Cousine-Regular.ttf",
		"DroidSans.ttf",
		"Karla-Regular.ttf",
		"ProggyClean.ttf",
		"ProggyTiny.ttf",
		"Roboto-Medium.ttf",
	}};

	constexpr float kDefaultFontSize = 16.0F;
	ImFont* defaultFont = nullptr;
	for (const auto* font : kFonts)
	{
		fontPath.replace_filename(font);
		defaultFont = imguiIO.Fonts->AddFontFromFileTTF(
			fontPath.generic_string().c_str(), kDefaultFontSize * std::max(dpiScaleX, dpiScaleY), &config);
	}

	// Setup style
	StyleColorsClassic();
	imguiIO.FontDefault = defaultFont;

	// we allow up to queue count in-flight renders per frame due to triple buffering. passed in rather than read here,
	// since the caller already holds the graphics queue write lock.
	gIMGUIRenderer = std::make_unique<ImGuiRenderer>(rhi.GetPrimaryDevice(), window, graphicsQueue, graphicsQueueCount);
	ImGui_ImplGlfw_InitForOther(reinterpret_cast<GLFWwindow*>(GetCurrentWindow()), true);

	// IMNODES_NAMESPACE::CreateContext();
	// IMNODES_NAMESPACE::LoadCurrentEditorStateFromIniString(
	//	myNodeGraph.layout.c_str(), myNodeGraph.layout.size());
}

static void ShutdownImgui()
{
	// size_t count;
	// myNodeGraph.layout.assign(IMNODES_NAMESPACE::SaveCurrentEditorStateToIniString(&count));
	// IMNODES_NAMESPACE::DestroyContext();

	gIMGUIRenderer.reset();
	ImGui_ImplGlfw_Shutdown();

	// snapshot draw lists are registered with the context's shared data, and must be gone before it is destroyed
	for (auto& frame : gIMGUIFrames)
	{
		frame.drawData.Clear();
		frame.snapshot.Clear();
	}

	ImGui::DestroyContext();
}

// what the descriptor sets of the pipeline (see shaders/capi.h) are allocated from: room for many copies of the
// global arrays, since every change to one takes a new descriptor set
static std::vector<DescriptorPoolSize> DescriptorPoolSizes()
{
	constexpr uint32_t kGlobalResourceBaseCount = 128;
	constexpr uint32_t kBufferBaseCount = kGlobalResourceBaseCount * 1024;

	return {
		{.type = rhi::DescriptorType::kSampler, .count = kGlobalResourceBaseCount * SHADER_TYPES_GLOBAL_SAMPLER_COUNT},
		{.type = rhi::DescriptorType::kCombinedImageSampler, .count = kGlobalResourceBaseCount * SHADER_TYPES_GLOBAL_SAMPLER_COUNT},
		{.type = rhi::DescriptorType::kSampledImage, .count = kGlobalResourceBaseCount * SHADER_TYPES_GLOBAL_TEXTURE_COUNT},
		{.type = rhi::DescriptorType::kStorageImage, .count = kGlobalResourceBaseCount * SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT},
		{.type = rhi::DescriptorType::kUniformTexelBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kStorageTexelBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kUniformBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kStorageBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kUniformBufferDynamic, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kStorageBufferDynamic, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kInputAttachment, .count = kBufferBaseCount},
	};
}

// gTextures slots 0 to SHADER_TYPES_FRAME_COUNT - 1 hold the frames' render targets (for ComputeMain). material 0 is the
// default material, for models (or parts of them) without one: it samples this slot, which "Open Image..." replaces.
static constexpr uint32_t kMaterialTextureId = 15;
// the loaded model's materials are 1 and up, and their textures are in the slots from here up
static constexpr uint32_t kModelTextureFirstSlot = 16;
static constexpr uint32_t kModelTextureMaxCount = SHADER_TYPES_GLOBAL_TEXTURE_COUNT - kModelTextureFirstSlot;
static constexpr uint32_t kModelMaterialMaxCount = SHADER_TYPES_MATERIAL_COUNT - 1;
static constexpr uint32_t kDefaultSamplerId = 2;
static_assert(kMaterialTextureId >= SHADER_TYPES_FRAME_COUNT && kMaterialTextureId < kModelTextureFirstSlot);

// the material slot drawn for a submesh of the loaded model
static uint32_t ModelMaterialSlot(int32_t material)
{
	return material >= 0 && static_cast<uint32_t>(material) < kModelMaterialMaxCount ? static_cast<uint32_t>(material) + 1 : 0;
}

// the loaded model's textures, by slot (from kModelTextureFirstSlot). nil for slots it doesn't use.
static std::vector<std::pair<uuids::uuid, uuids::uuid>> gModelTextureUuids; // image, view

// hands `resource` to a graphics queue submission that waits for all graphics work submitted so far, and releases it
// from that submission's timeline callback, i.e. once the gpu can no longer be using it. call on the draw thread.
static void RetireAfterGraphicsWork(QueueTimelineContextData& graphics, std::shared_ptr<void> resource)
{
	if (!resource)
		return;

	auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();

	auto cmd = graphicsQueue.GetPool().Commands();
	cmd.End();

	std::vector<core::TaskHandle> callbacks;
	callbacks.emplace_back(core::CreateTask([resource = std::move(resource)] {}).handle);

	graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {PipelineStage::kAllCommands},
		.waitSemaphoreValues = {graphics.timeline},
		.signalSemaphores = {graphics.semaphore},
		.signalSemaphoreValues = {++graphics.timeline},
		.callbacks = std::move(callbacks)});

	graphicsSubmits |= graphicsQueue.Submit();
}

// writes materials, starting at slot first, in a submission on the graphics queue that is ordered after all graphics
// work submitted so far, and before all that follows. call on the draw thread.
static void UpdateMaterials(
	RHI& rhi, QueueTimelineContextData& graphics, uint32_t first, std::span<const MaterialData> materials)
{
	if (materials.empty())
		return;

	ENSURE(first + materials.size() <= SHADER_TYPES_MATERIAL_COUNT);

	auto& device = rhi.GetPrimaryDevice();
	auto& buffer = *device.GetResource<Buffer>(gMaterialsUuid);
	auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();

	auto cmd = graphicsQueue.GetPool().Commands();
	{
		GPU_SCOPE(cmd, graphicsQueue, UpdateMaterials); //NOLINT(bugprone-suspicious-stringview-data-usage)

		CommandEncoder encoder(cmd);

		// after the frames in flight have read the old ones
		encoder.Barrier(PipelineStage::kAllCommands, Access::kShaderRead, PipelineStage::kTransfer, Access::kTransferWrite);

		// 32 kB at most, below UpdateBuffer's limit of 64 kB
		static_assert(SHADER_TYPES_MATERIAL_COUNT * sizeof(MaterialData) <= 65536);
		encoder.UpdateBuffer(buffer, first * sizeof(MaterialData), std::as_bytes(materials));

		encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kAllCommands, Access::kShaderRead);
	}
	cmd.End();

	graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {PipelineStage::kAllCommands},
		.waitSemaphoreValues = {graphics.timeline},
		.signalSemaphores = {graphics.semaphore},
		.signalSemaphoreValues = {++graphics.timeline}});

	graphicsSubmits |= graphicsQueue.Submit();
}

// buffers and images uploaded by the loaders (see Upload), to install
struct Uploads
{
	std::vector<std::pair<const Buffer*, Upload>> buffers;
	std::vector<std::pair<std::shared_ptr<Image>, Upload>> images;
};

// in a graphics queue submission that waits for the uploads, acquires the uploaded resources for the graphics queue
// family and transitions the images to a shader readable layout. once that has executed, has the draw thread call
// bind (with the graphics queue context). call on the draw thread.
static void TransitionThenBind(
	RHI& rhi,
	QueueTimelineContextData& graphics,
	const Uploads& uploads,
	std::function<void(QueueTimelineContextData&)> bind)
{
	auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();

	// what to wait for: all graphics work so far, and the latest upload on each other semaphore. uploads on the graphics
	// semaphore (the transfer queue type aliases the graphics queue's context) are covered by the first.
	QueueDeviceSyncInfo syncInfo{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {PipelineStage::kAllCommands},
		.waitSemaphoreValues = {graphics.timeline}};
	std::vector<const Semaphore*> waitSemaphores{&graphics.semaphore};
	auto waitFor = [&](const Upload& upload)
	{
		ENSURE(upload.semaphore != nullptr);
		auto it = std::ranges::find(waitSemaphores, upload.semaphore);
		auto index = static_cast<size_t>(it - waitSemaphores.begin());
		if (it == waitSemaphores.end())
		{
			waitSemaphores.push_back(upload.semaphore);
			syncInfo.waitSemaphores.emplace_back(*upload.semaphore);
			syncInfo.waitDstStageMasks.emplace_back(PipelineStage::kAllCommands);
			syncInfo.waitSemaphoreValues.emplace_back(upload.value);
		}
		else if (index > 0)
			syncInfo.waitSemaphoreValues[index] = std::max(syncInfo.waitSemaphoreValues[index], upload.value);
	};

	auto cmd = graphicsQueue.GetPool().Commands();
	{
		GPU_SCOPE(cmd, graphicsQueue, Transition); //NOLINT(bugprone-suspicious-stringview-data-usage)

		CommandEncoder encoder(cmd);
		for (const auto& [buffer, upload] : uploads.buffers)
		{
			waitFor(upload);
			encoder.AcquireOwnership(
				*buffer,
				upload.queueFamilyIndex,
				graphics.queueFamilyIndex,
				PipelineStage::kAllCommands,
				Access::kShaderRead | Access::kIndexRead);
		}

		for (const auto& [image, upload] : uploads.images)
		{
			waitFor(upload);
			// in the upload's layout, which the transition below starts from
			encoder.AcquireOwnership(
				*image, upload.queueFamilyIndex, graphics.queueFamilyIndex, PipelineStage::kAllCommands, Access::kShaderRead);
			image->Transition(cmd, ImageLayout::kShaderReadOnly);
		}
	}
	cmd.End();

	auto [transitionDoneTask, transitionDoneFuture] = core::CreateTask([&rhi, bind = std::move(bind)]
	{
		auto [bindTask, bindFuture] = core::CreateTask<QueueTimelineContextData*>(
			[bind](QueueTimelineContextData* graphics) { bind(*graphics); });
		rhi.drawCalls.enqueue(bindTask);
	});

	syncInfo.signalSemaphores = {graphics.semaphore};
	syncInfo.signalSemaphoreValues = {++graphics.timeline};
	syncInfo.callbacks.emplace_back(transitionDoneTask);
	graphicsQueue.EnqueueSubmit(std::move(syncInfo));

	graphicsSubmits |= graphicsQueue.Submit();
}

// a material's textures, null where it has none (or it failed to load)
struct MaterialTextures
{
	Texture diffuse;
	Texture alpha;
	Texture normal;
};

// makes an uploaded model the one being drawn, with its materials and their textures (by material), retiring the
// previous ones. call on the draw thread.
static void InstallModel(
	RHI& rhi,
	QueueTimelineContextData& graphics,
	const std::shared_ptr<Model>& model,
	std::vector<MaterialTextures> textures)
{
	ZoneScopedN("WindowedApplication::InstallModel");

	// each resource once: materials share textures
	Uploads uploads;
	for (const auto* buffer : {&model->GetIndexBuffer(), &model->GetVertexBuffer()})
		uploads.buffers.emplace_back(buffer, model->GetUpload());
	for (const auto& material : textures)
		for (const auto* texture : {&material.diffuse, &material.alpha, &material.normal})
			if (texture->image &&
				std::ranges::none_of(uploads.images, [&texture](const auto& image) { return image.first == texture->image; }))
				uploads.images.emplace_back(texture->image, texture->upload);

	// everything is switched at once, after the textures are readable: one descriptor set update, and no frame draws
	// the new model with the old materials or the other way around
	TransitionThenBind(rhi, graphics, uploads, [&rhi, model, textures = std::move(textures)](QueueTimelineContextData& graphics)
	{
		auto& device = rhi.GetPrimaryDevice();
		auto& pipeline = device.GetPipeline();
		const auto& blackView = *device.GetResource<ImageView>(gBlackTextureViewUuid);

		pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);

		// each texture once, in the slots from kModelTextureFirstSlot. the previous model's slots go back to black.
		std::vector<std::pair<uuids::uuid, uuids::uuid>> textureUuids;
		core::UnorderedMap<const Image*, uint32_t> textureSlots;
		auto slotOf = [&](const Texture& texture) -> std::optional<uint32_t>
		{
			const auto& [image, view, upload] = texture;
			if (!image)
				return std::nullopt;

			if (auto it = textureSlots.find(image.get()); it != textureSlots.end())
				return it->second;

			if (textureUuids.size() == kModelTextureMaxCount)
				return std::nullopt;

			auto slot = static_cast<uint32_t>(kModelTextureFirstSlot + textureUuids.size());
			pipeline.SetDescriptorData(
				"gTextures",
				ImageBinding{.sampler = {}, .imageView = *view, .layout = image->GetDesc().layout},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				slot);

			device.AddResource(image);
			device.AddResource(view);
			textureUuids.emplace_back(image->GetUuid(), view->GetUuid());
			textureSlots.emplace(image.get(), slot);

			return slot;
		};

		std::vector<MaterialData> materials(std::min<size_t>(textures.size(), kModelMaterialMaxCount));
		for (size_t materialIt = 0; materialIt < materials.size(); materialIt++)
		{
			auto& material = materials[materialIt];
			std::ranges::fill(material.color, 1.0F);
			material.textureAndSamplerId = kDefaultSamplerId;
			material.alphaCutoff = model->GetDesc().materials[materialIt].alphaCutoff;

			if (auto slot = slotOf(textures[materialIt].diffuse))
			{
				material.textureAndSamplerId |= *slot << SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS;
				material.flags |= MATERIAL_FLAG_TEXTURE;
			}
			if (auto slot = slotOf(textures[materialIt].alpha))
			{
				material.alphaTextureId = *slot;
				material.flags |= MATERIAL_FLAG_ALPHA_TEXTURE;
			}
			if (auto slot = slotOf(textures[materialIt].normal))
			{
				material.normalTextureId = *slot;
				material.flags |= MATERIAL_FLAG_NORMAL_TEXTURE;
			}
		}

		for (size_t slotIt = textureUuids.size(); slotIt < gModelTextureUuids.size(); slotIt++)
			pipeline.SetDescriptorData(
				"gTextures",
				ImageBinding{.sampler = {}, .imageView = blackView, .layout = ImageLayout::kShaderReadOnly},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				kModelTextureFirstSlot + slotIt);

		for (const auto& [imageUuid, viewUuid] : gModelTextureUuids)
		{
			RetireAfterGraphicsWork(graphics, device.ReplaceResource(viewUuid, nullptr));
			RetireAfterGraphicsWork(graphics, device.ReplaceResource(imageUuid, nullptr));
		}
		gModelTextureUuids = std::move(textureUuids);

		UpdateMaterials(rhi, graphics, 1, materials);

		pipeline.SetDescriptorData(
			"gVertexBuffer",
			BufferBinding{.buffer = model->GetVertexBuffer(), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

		RetireAfterGraphicsWork(graphics, std::exchange(gModel, model));

		App().GetViews().FrameBounds(model->GetDesc().bounds);
	});
}

// makes an uploaded image the texture sampled by material 0, retiring the previous one. call on the draw thread.
static void InstallImage(
	RHI& rhi,
	QueueTimelineContextData& graphics,
	const std::shared_ptr<Image>& image,
	const std::shared_ptr<ImageView>& imageView,
	const Upload& upload)
{
	ZoneScopedN("WindowedApplication::InstallImage");

	TransitionThenBind(rhi, graphics, Uploads{.images = {{image, upload}}}, [&rhi, image, imageView](QueueTimelineContextData& graphics)
	{
		auto& device = rhi.GetPrimaryDevice();
		auto& pipeline = device.GetPipeline();

		pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);
		pipeline.SetDescriptorData(
			"gTextures",
			ImageBinding{.sampler = {}, .imageView = *imageView, .layout = image->GetDesc().layout},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			kMaterialTextureId);

		// the default material is untextured until an image is loaded
		MaterialData material{
			.color = {1.0F, 1.0F, 1.0F, 1.0F},
			.textureAndSamplerId = (kMaterialTextureId << SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS) | kDefaultSamplerId,
			.flags = MATERIAL_FLAG_TEXTURE,
			.alphaCutoff = 0.5F};
		UpdateMaterials(rhi, graphics, 0, std::span(&material, 1));

		RetireAfterGraphicsWork(graphics, device.ReplaceResource(gLoadedImageUuid, image));
		RetireAfterGraphicsWork(graphics, device.ReplaceResource(gLoadedImageViewUuid, imageView));
		gLoadedImageUuid = image->GetUuid();
		gLoadedImageViewUuid = imageView->GetUuid();
	});
}

// loads a model (or several, side by side as one, see Model::Load) and its materials' textures, and has the draw thread
// install them, unless the load was cancelled. call from a load (see gLoads).
static void LoadAndInstallModels(RHI& rhi, const std::vector<std::string>& filePaths, std::atomic_uint8_t& progress)
{
	auto model = Model::Load(std::vector<std::string_view>(filePaths.begin(), filePaths.end()), progress);
	if (!model) // cancelled or failed
		return;

	const auto& filePath = model->GetDesc().name; // or the models' directory, for several

	const auto& materials = model->GetDesc().materials;
	if (materials.size() > kModelMaterialMaxCount)
		std::println(stderr, "{}: {} materials, only the first {} are used", filePath, materials.size(), kModelMaterialMaxCount);

	// then the textures, each file and usage once. the progress starts over for them.
	struct TextureLoad
	{
		std::string path;
		gfx::image::Options options;
		Texture* result;
	};
	std::vector<MaterialTextures> textures(materials.size());
	std::vector<TextureLoad> loads;
	for (size_t materialIt = 0; materialIt < materials.size(); materialIt++)
	{
		const auto& material = materials[materialIt];
		auto& texture = textures[materialIt];
		if (!material.diffuseTexture.empty())
			loads.push_back({material.diffuseTexture, {.usage = gfx::image::Usage::kColor}, &texture.diffuse});
		if (!material.alphaTexture.empty())
			loads.push_back({material.alphaTexture, {.usage = gfx::image::Usage::kMask}, &texture.alpha});
		if (!material.normalTexture.empty())
			loads.push_back({material.normalTexture, {.usage = gfx::image::Usage::kNormal}, &texture.normal});
		else if (!material.bumpTexture.empty())
			loads.push_back(
				{material.bumpTexture, {.usage = gfx::image::Usage::kBump, .bumpScale = material.bumpScale}, &texture.normal});
	}

	core::UnorderedMap<std::string, Texture> loaded;
	progress = 0;
	for (size_t loadIt = 0; loadIt < loads.size(); loadIt++)
	{
		const auto& load = loads[loadIt];
		auto key = std::format("{}|{}|{}", load.path, std::to_underlying(load.options.usage), load.options.bumpScale);
		auto [it, inserted] = loaded.try_emplace(std::move(key));
		if (inserted)
		{
			if (core::Application::Get()->IsExitRequested())
				return;

			std::atomic_uint8_t textureProgress = 0;
			it->second = LoadTexture(load.path, textureProgress, load.options);
		}
		*load.result = it->second;

		progress = static_cast<uint8_t>(255 * (loadIt + 1) / loads.size());
	}

	if (loaded.size() > kModelTextureMaxCount)
		std::println(stderr, "{}: {} textures, only the first {} are used", filePath, loaded.size(), kModelTextureMaxCount);

	auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData*>(
		[&rhi, model, textures = std::move(textures)](QueueTimelineContextData* graphics) mutable
		{ InstallModel(rhi, *graphics, model, std::move(textures)); });
	rhi.drawCalls.enqueue(installTask);
}

static void LoadAndInstallModel(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress)
{
	LoadAndInstallModels(rhi, {std::string(filePath)}, progress);
}

// extracts a zip archive into the user profile directory, once: later loads of the same file (by path, size and time)
// reuse it. returns the directory it was extracted to, or nothing if it failed (the reason is printed to stderr) or was
// cancelled.
static std::optional<std::filesystem::path> ExtractArchive(const std::filesystem::path& archive, std::atomic_uint8_t& progress)
{
	ZoneScopedN("WindowedApplication::ExtractArchive");

	auto app = core::Application::Get();
	auto userProfilePath = std::get<std::filesystem::path>(app->GetEnv().variables["UserProfilePath"]);

	std::error_code error;
	auto absolute = std::filesystem::absolute(archive, error);
	auto size = std::filesystem::file_size(archive, error);
	auto time = std::filesystem::last_write_time(archive, error).time_since_epoch().count();
	if (error)
	{
		std::println(stderr, "Failed to load archive {}: {}", archive.string(), error.message());
		return std::nullopt;
	}

	auto key = std::hash<std::string>{}(std::format("{}|{}|{}", absolute.string(), size, time));
	auto directory = userProfilePath / "archives" / std::format("{}-{:016x}", archive.stem().string(), key);
	auto marker = directory / ".extracted";
	if (std::filesystem::exists(marker, error))
		return directory;

	// a previous extraction that didn't finish
	std::filesystem::remove_all(directory, error);

	auto result = gfx::zip::ExtractAll(archive, directory, &progress, [&app] { return app->IsExitRequested(); });
	if (!result)
	{
		if (!app->IsExitRequested())
			std::println(stderr, "Failed to load archive {}: {}", archive.string(), result.error());
		std::filesystem::remove_all(directory, error);
		return std::nullopt;
	}

	std::ofstream(marker).put('\n');

	return directory;
}

// the model files (.obj, .gltf, .glb) below a directory, sorted
static std::vector<std::filesystem::path> FindModels(const std::filesystem::path& directory)
{
	std::vector<std::filesystem::path> models;
	std::error_code error;
	for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
		 !error && it != std::filesystem::recursive_directory_iterator();
		 it.increment(error))
	{
		if (mesh::IsModelFile(it->path()) && it->is_regular_file(error))
			models.push_back(it->path());
	}
	std::ranges::sort(models);
	return models;
}

// the models of an archive with several, for the user to choose from (see PrepareDraw)
struct ArchiveChoice
{
	std::string archive;
	std::filesystem::path directory;
	std::vector<std::filesystem::path> models;
};
static std::mutex gArchiveChoiceMutex;
static std::optional<ArchiveChoice> gArchiveChoice; // guarded by gArchiveChoiceMutex

// what LoadAndInstallArchive does with an archive of several models (sets of variants, such as the geodesic spheres)
enum class ArchiveModels : uint8_t
{
	kChoose, // the user chooses one, or all of them
	kAll, // all of them, side by side
};

// extracts a zip archive and loads the model in it. with several, see ArchiveModels. call from a load (see gLoads).
static void LoadAndInstallArchive(RHI& rhi, std::string_view archivePath, std::atomic_uint8_t& progress, ArchiveModels several)
{
	auto directory = ExtractArchive(archivePath, progress);
	if (!directory) // failed or cancelled
		return;

	auto models = FindModels(*directory);
	if (models.empty())
	{
		std::println(stderr, "Failed to load archive {}: it holds no model files", archivePath);
		return;
	}

	if (models.size() == 1 || several == ArchiveModels::kAll)
	{
		progress = 0;
		std::vector<std::string> paths;
		for (const auto& model : models)
			paths.push_back(model.string());
		LoadAndInstallModels(rhi, paths, progress);
		return;
	}

	std::scoped_lock lock(gArchiveChoiceMutex);
	gArchiveChoice = ArchiveChoice{
		.archive = std::filesystem::path(archivePath).filename().string(),
		.directory = std::move(*directory),
		.models = std::move(models)};
}

// loads an image and has the draw thread install it, unless the load was cancelled. call from a load (see gLoads).
static void LoadAndInstallImage(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress)
{
	auto [image, imageView, upload] = LoadTexture(filePath, progress);
	if (!image) // cancelled or failed
		return;

	auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData*>(
		[&rhi, image, imageView, upload](QueueTimelineContextData* graphics)
		{ InstallImage(rhi, *graphics, image, imageView, upload); });
	rhi.drawCalls.enqueue(installTask);
}

static void DrawMainPass(
	RHI& rhi,
	Window& window,
	Pipeline& pipeline,
	Queue& graphicsQueue,
	CommandBufferHandle cmd,
	uint16_t newFrameIndex,
	uint64_t graphicsTimeline)
{
	GPU_SCOPE(cmd, graphicsQueue, draw);

	auto& device = rhi.GetPrimaryDevice();
	auto& renderImageSet = *device.GetResource<RenderImageSet>(gRenderImageSetUuids[newFrameIndex]);

	renderImageSet.SetLoadOp(LoadOp::kClear, 0);
	renderImageSet.SetLoadOp(LoadOp::kClear, renderImageSet.GetAttachments().size() - 1, LoadOp::kClear);
	renderImageSet.SetStoreOp(StoreOp::kStore, 0);
	renderImageSet.SetStoreOp(StoreOp::kStore, renderImageSet.GetAttachments().size() - 1, StoreOp::kStore);
	renderImageSet.Transition(cmd, ImageLayout::kColorAttachment, ImageAspect::kColor, 0);
	renderImageSet.Transition(cmd, ImageLayout::kDepthStencilAttachment, ImageAspect::kDepth | ImageAspect::kStencil, renderImageSet.GetAttachments().size() - 1);

	pipeline.SetRenderTarget(renderImageSet);

	auto renderTargetInfo = renderImageSet.Begin(cmd, SubpassContents::kSecondaryCommandBuffers);

	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);

	// setup draw parameters
	const auto grid = App().GetViews().GetGrid();
	uint32_t drawCount = grid.x * grid.y;
	uint32_t drawThreadCount = 0;

	std::atomic_uint32_t drawAtomic = 0UL;

	// draw views using secondary command buffers
	// todo: generalize this to other types of draws
	if (gModel)
	{
		auto& model = *gModel;

		ZoneScopedN("WindowedApplication::Draw::drawViews");

		drawThreadCount = std::min<uint32_t>(drawCount, graphicsQueue.GetPool().GetDesc().levelCount);

		constexpr uint32_t kMaxDrawThreads = 128;
		std::array<uint32_t, kMaxDrawThreads> seq;
		std::iota(seq.begin(), seq.begin() + drawThreadCount, 0);
		std::for_each_n(
			seq.begin(),
			drawThreadCount,
			[&pipeline,
			&graphicsQueue,
			&renderTargetInfo,
			&renderImageSet,
			&newFrameIndex,
			&drawAtomic,
			&drawCount,
			&model,
			grid](uint32_t threadIt)
			{
				ZoneScoped;

				auto drawIt = drawAtomic++;
				if (drawIt >= drawCount)
					return;

				auto zoneNameStr = std::format("Window::drawPartition thread:{}", threadIt);

				ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

				const auto extent = renderImageSet.GetExtent();
				uint32_t deltaX = extent.width / grid.x;
				uint32_t deltaY = extent.height / grid.y;

				auto cmd = graphicsQueue.GetPool().SecondaryCommands(threadIt + 1, renderTargetInfo);
				CommandEncoder encoder(cmd);

				auto bindState = [&pipeline, &model, &encoder](CommandBufferHandle cmd)
				{
					ZoneScopedN("bindState");

					// vertices are pulled from gVertexBuffer by SV_VertexID, so there is no vertex input state to bind
					encoder.BindIndexBuffer(model.GetIndexBuffer(), 0, IndexType::kUint32);

					// bind descriptor sets
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_VIEW);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MATERIAL);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

					// bind pipeline and buffers
					pipeline.BindPipelineAuto(cmd);
				};

				bindState(cmd);

				PushConstants pushConstants{.frameIndex = newFrameIndex};

				ASSERT(deltaX > 0);
				ASSERT(deltaY > 0);

				while (drawIt < drawCount)
				{
					auto drawView = [&pushConstants, &pipeline, &model, &cmd, &encoder, &deltaX, &deltaY, grid](uint16_t viewIt)
					{
						ZoneScopedN("drawView");

						uint32_t col = viewIt % grid.x;
						uint32_t row = viewIt / grid.x;

						{
							ZoneScopedN("setViewportAndScissor");

							ASSERT(deltaX > 0);
							ASSERT(deltaY > 0);

							auto posX = static_cast<int32_t>(col * deltaX);
							auto posY = static_cast<int32_t>(row * deltaY);
							encoder.SetViewport(Viewport{
								.x = static_cast<float>(posX),
								.y = static_cast<float>(posY),
								.width = static_cast<float>(deltaX),
								.height = static_cast<float>(deltaY),
								.minDepth = 0.0F,
								.maxDepth = 1.0F});
							encoder.SetScissor(rhi::Rect{.x = posX, .y = posY, .width = deltaX, .height = deltaY});
						}

						uint16_t viewIndex = viewIt;
						constexpr uint32_t kDefaultModelInstanceId = 666;

						pushConstants.modelInstanceId = kDefaultModelInstanceId;

						// one draw per material (see InstallModel for where the materials are)
						auto drawModel = [&pushConstants, &pipeline, &model, &encoder, viewIndex](CommandBufferHandle cmd)
						{
							ZoneScopedN("drawModel");

							for (const auto& submesh : model.GetDesc().submeshes)
							{
								pushConstants.viewAndMaterialId =
									(static_cast<uint32_t>(viewIndex) << SHADER_TYPES_MATERIAL_INDEX_BITS) | ModelMaterialSlot(submesh.material);

								pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));

								encoder.DrawIndexed(submesh.indexCount, 1, submesh.firstIndex);
							}
						};

						drawModel(cmd);
					};

					drawView(drawIt);

					drawIt = drawAtomic++;
				}

				cmd.End();
			});
	}

	for (uint32_t threadIt = 1UL; threadIt <= drawThreadCount; threadIt++)
		graphicsQueue.Execute(threadIt, graphicsTimeline);

	renderImageSet.End(cmd);
}

void CreateWindowDependentObjects(RHI& rhi)
{
	ZoneScopedN("CreateWindowDependentObjects");
	
	auto& device = rhi.GetPrimaryDevice();
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto frameCount = window.GetSwapchain().GetFrames().size();
	ENSURE(frameCount <= gRenderImageSetUuids.size());
	
	for (unsigned frameIt = 0; frameIt < frameCount; frameIt++)
	{
		auto colorImage = Image(
			ImageCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("Main RT Color Image {}", frameIt)),
				{{.extent = window.GetSwapchain().GetDesc().extent}},
				// linear: shading and blending happen in linear space, and ComputeMain applies the srgb curve when it
				// copies the result to the swapchain. vulkan requires this format to support all of the usages below.
				Format::kR16G16B16A16Sfloat,
				ImageTiling::kOptimal,
				ImageUsage::kColorAttachment | ImageUsage::kTransferSource | ImageUsage::kSampled | ImageUsage::kStorage,
				MemoryProperty::kDeviceLocal,
				ImageAspect::kColor,
				ImageLayout::kUndefined});

		auto depthStencilImage = Image(
			ImageCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("Main RT DepthStencil Image {}", frameIt)),
				{{.extent = window.GetSwapchain().GetDesc().extent}},
				device.FindSupportedFormat(
					std::array{Format::kD32SfloatS8Uint, Format::kD24UnormS8Uint},
					ImageTiling::kOptimal,
					FormatFeature::kDepthStencilAttachment | FormatFeature::kTransferSource |
						FormatFeature::kTransferDestination),
				ImageTiling::kOptimal,
				ImageUsage::kDepthStencilAttachment | ImageUsage::kSampled,
				MemoryProperty::kDeviceLocal,
				ImageAspect::kDepth | ImageAspect::kStencil,
				ImageLayout::kUndefined});

		// one render target per frame, replacing any previous one (e.g. on resize)
		device.EraseResource(gRenderImageSetUuids[frameIt]);
		gRenderImageSetUuids[frameIt] =
			device.CreateResource<RenderImageSet>(std::move(colorImage), std::move(depthStencilImage))->GetUuid();
	}

	{
		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
		
		auto cmd = graphicsQueue.GetPool().Commands();

		// no layout transitions for the swapchain images here: they may only be used once acquired. Draw transitions
		// each acquired image from its tracked layout (UNDEFINED for a new swapchain).
		for (auto& frame : window.GetSwapchain().GetFrames())
		{
			frame.SetLoadOp(LoadOp::kClear, 0);
			frame.SetStoreOp(StoreOp::kStore, 0);
		}

		for (const auto& renderImageSetGuid : std::span(gRenderImageSetUuids).first(frameCount))
		{
			auto& renderImageSet = *device.GetResource<RenderImageSet>(renderImageSetGuid);
			renderImageSet.SetLoadOp(LoadOp::kClear, 0);
			renderImageSet.SetLoadOp(LoadOp::kClear, renderImageSet.GetAttachments().size() - 1, LoadOp::kClear);
			renderImageSet.SetStoreOp(StoreOp::kStore, 0);
			renderImageSet.SetStoreOp(StoreOp::kStore, renderImageSet.GetAttachments().size() - 1, StoreOp::kStore);
			renderImageSet.Transition(cmd, ImageLayout::kGeneral, ImageAspect::kColor, 0);
			renderImageSet.Transition(cmd, ImageLayout::kGeneral, ImageAspect::kDepth | ImageAspect::kStencil, renderImageSet.GetAttachments().size() - 1);
		}

		cmd.End();

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
			.waitSemaphores = {},
			.waitDstStageMasks = {},
			.waitSemaphoreValues = {},
			.signalSemaphores = {graphics->semaphore},
			.signalSemaphoreValues = {++graphics->timeline}});

		graphicsSubmits |= graphicsQueue.Submit();
	}

	// Draw only writes the current frame's element of these arrays, but a dirty set is updated as a whole: after a
	// resize the other frames' elements would still reference the destroyed views. so write all of them up front,
	// with the layouts Draw uses (so Draw's own writes are skipped as unchanged).
	auto& pipeline = device.GetPipeline();
	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kCompute);
	for (unsigned frameIt = 0; frameIt < frameCount; frameIt++)
	{
		auto& renderImageSet = *device.GetResource<RenderImageSet>(gRenderImageSetUuids[frameIt]);
		auto& frame = window.GetSwapchain().GetFrames()[frameIt];

		pipeline.SetDescriptorData(
			"gTextures",
			ImageBinding{
				.sampler={},
				.imageView=renderImageSet.GetAttachments()[0],
				.layout=ImageLayout::kShaderReadOnly},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			frameIt);

		pipeline.SetDescriptorData(
			"gRWTextures",
			ImageBinding{
				.sampler={},
				.imageView=frame.GetAttachments()[0],
				.layout=ImageLayout::kGeneral},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
			frameIt);
	}
}

// recreates the swapchain at the current surface size, and everything sized after it. caller holds gDrawMutex.
// returns false if the surface has no area (minimized), in which case nothing is recreated.
bool RecreateWindowDependentObjects(RHI& rhi, Window& window)
{
	ZoneScopedN("RecreateWindowDependentObjects");

	auto& device = rhi.GetPrimaryDevice();

	auto extent = window.GetSwapchain().QuerySurfaceExtent();
	if (extent.width == 0 || extent.height == 0)
		return false;

	device.WaitIdle();
	window.OnResizeFramebuffer(static_cast<int>(extent.width), static_cast<int>(extent.height));
	App().GetViews().OnResizeFramebuffer({extent.width, extent.height});
	App().GetViews().UpdateBuffers();
	CreateWindowDependentObjects(rhi);

	return true;
}

} // namespace windowedapplication

void WindowedApplication::PrepareDraw()
{
	ZoneScopedN("WindowedApplication::PrepareDraw");

	using namespace windowedapplication;
	using namespace ImGui;

	auto& rhi = GetRHI();
	auto& device = rhi.GetPrimaryDevice();

	ImGui_ImplGlfw_NewFrame(); // will poll glfw input events and update input state
	gIMGUIRenderer->NewFrame();
	NewFrame();

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	static bool gShowStatistics = false;
	{
		if (gShowStatistics)
		{
			if (Begin("Statistics", &gShowStatistics))
			{
				for (const auto& [name, count] : GetObjectCounts<kGraphicsApi>())
					Text("%.*s: %u", static_cast<int>(name.size()), name.data(), count);

				// gpu submits: totals, and how many went to the gpu between the last two presented frames (see Draw)
				Separator();
				Text(
					"Queue Submits: %llu (%u/frame)",
					static_cast<unsigned long long>(Queue::GetSubmitCount()),
					gFrameSubmitCount.load(std::memory_order_relaxed));
				Text(
					"Submit Batches: %llu (%u/frame)",
					static_cast<unsigned long long>(Queue::GetSubmitBatchCount()),
					gFrameSubmitBatchCount.load(std::memory_order_relaxed));
			}
			End();
		}
	}
#endif

	// the models of an archive with several (see LoadAndInstallArchive)
	static std::optional<ArchiveChoice> gShownArchiveChoice;
	if (!gShownArchiveChoice)
	{
		std::scoped_lock lock(gArchiveChoiceMutex);
		if (gArchiveChoice)
		{
			gShownArchiveChoice = std::move(gArchiveChoice);
			gArchiveChoice.reset();
			OpenPopup("Load Model");
		}
	}
	if (BeginPopupModal("Load Model", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		const auto& choice = *gShownArchiveChoice;
		Text("%s holds %zu models:", choice.archive.c_str(), choice.models.size());

		constexpr float kListHeight = 300.0F;
		std::optional<std::filesystem::path> chosen;
		if (BeginChild("Models", ImVec2(0.0F, kListHeight), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_Borders))
		{
			for (const auto& model : choice.models)
				if (Selectable(model.lexically_relative(choice.directory).string().c_str()))
					chosen = model;
		}
		EndChild();

		if (chosen)
			(void)gLoads.Enqueue(
				chosen->filename().string(),
				[&rhi, path = chosen->string()](std::atomic_uint8_t& progress) { LoadAndInstallModel(rhi, path, progress); });

		bool all = Button("All, side by side");
		if (all)
		{
			std::vector<std::string> paths;
			for (const auto& model : choice.models)
				paths.push_back(model.string());
			(void)gLoads.Enqueue(
				choice.archive,
				[&rhi, paths = std::move(paths)](std::atomic_uint8_t& progress) { LoadAndInstallModels(rhi, paths, progress); });
		}
		SameLine();

		if (chosen || all || Button("Cancel"))
		{
			gShownArchiveChoice.reset();
			CloseCurrentPopup();
		}
		EndPopup();
	}

	if (gShowDemoWindow)
		ShowDemoWindow(&gShowDemoWindow);

	if (gShowAbout && Begin("About client", &gShowAbout))
	{
		End();
	}

	// ticks per second: PrepareDraw runs once per tick, measured like the frame rate (see Draw)
	static float gTicksPerSecond = 0.0F;
	{
		using namespace std::chrono_literals;
		static auto gTpsTime = std::chrono::steady_clock::now();
		static uint32_t gTpsTickCount = 0;
		gTpsTickCount++;
		if (auto now = std::chrono::steady_clock::now(); now - gTpsTime >= 500ms)
		{
			gTicksPerSecond = static_cast<float>(gTpsTickCount) / std::chrono::duration<float>(now - gTpsTime).count();
			gTpsTickCount = 0;
			gTpsTime = now;
		}
	}

	// frame/tick rate overlay in the top right corner, below the menu bar
	if (gShowFps || gShowTps)
	{
		constexpr float kFpsOverlayPadding = 10.0F;
		constexpr float kFpsOverlayBgAlpha = 0.35F;
		const auto* viewport = GetMainViewport();
		SetNextWindowPos(
			ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - kFpsOverlayPadding, viewport->WorkPos.y + kFpsOverlayPadding),
			ImGuiCond_Always,
			ImVec2(1.0F, 0.0F));
		SetNextWindowBgAlpha(kFpsOverlayBgAlpha);
		if (Begin(
				"Rates",
				nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
		{
			if (gShowFps)
			{
				float fps = gFramesPerSecond.load(std::memory_order_relaxed);
				Text("%.0f FPS (%.2f ms)", fps, fps > 0.0F ? 1000.0F / fps : 0.0F);
			}
			if (gShowTps)
				Text("%.0f TPS (%.2f ms)", gTicksPerSecond, gTicksPerSecond > 0.0F ? 1000.0F / gTicksPerSecond : 0.0F);
		}
		End();
	}

	// one row per load in progress
	if (bool loading = !gLoads.Empty() &&
				 Begin(
					 "Loading",
					 &loading,
					 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration |
						 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
						 ImGuiWindowFlags_AlwaysAutoResize))
	{
		constexpr uint8_t kProgressMax = 255;
		constexpr float kProgressBarWidth = 160.0F;
		gLoads.ForEach([](const core::LoadQueue::Load& load)
		{
			ProgressBar((1.F / kProgressMax) * static_cast<float>(load.progress), ImVec2(kProgressBarWidth, 0));
			SameLine();
			TextUnformatted(load.name.c_str());
		});
		End();
	}

	auto resourcePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]);
	auto& window = rhi.GetWindow(GetCurrentWindow());

	// automation: SPEEDO_AUTOLOAD_MODEL / SPEEDO_AUTOLOAD_IMAGE name a file in resources/models / resources/images (or
	// an absolute path; for a model also a zip archive) to load at startup, through the same load + install path as the "File" menu. with
	// SPEEDO_AUTOLOAD_EXIT=<frames>, the application exits that many frames after the loads have finished (see
	// scripts/assettest.sh).
	static std::vector<core::Future<void>> gAutoLoads;
	static std::optional<uint32_t> gAutoLoadExitFrames;
	if (static bool gAutoLoadDone = false; !gAutoLoadDone)
	{
		gAutoLoadDone = true;

		// queued as separate loads, which run concurrently
		// a zip archive loads all of its models, side by side
		if (const char* autoLoadModel = std::getenv("SPEEDO_AUTOLOAD_MODEL"); autoLoadModel != nullptr && *autoLoadModel != '\0')
			gAutoLoads.emplace_back(gLoads.Enqueue(
				autoLoadModel,
				[&rhi, path = (resourcePath / "models" / autoLoadModel).string()](std::atomic_uint8_t& progress)
				{
					if (std::string_view(path).ends_with(".zip") || std::string_view(path).ends_with(".ZIP"))
						LoadAndInstallArchive(rhi, path, progress, ArchiveModels::kAll);
					else
						LoadAndInstallModel(rhi, path, progress);
				}));
		if (const char* autoLoadImage = std::getenv("SPEEDO_AUTOLOAD_IMAGE"); autoLoadImage != nullptr && *autoLoadImage != '\0')
			gAutoLoads.emplace_back(gLoads.Enqueue(
				autoLoadImage,
				[&rhi, path = (resourcePath / "images" / autoLoadImage).string()](std::atomic_uint8_t& progress)
				{ LoadAndInstallImage(rhi, path, progress); }));
		if (const char* autoLoadExit = std::getenv("SPEEDO_AUTOLOAD_EXIT"); autoLoadExit != nullptr && *autoLoadExit != '\0')
			gAutoLoadExitFrames = static_cast<uint32_t>(std::strtoul(autoLoadExit, nullptr, 10));
	}

	// the installs are queued to the draw thread when the loads finish, so they have happened a frame later
	if (gAutoLoadExitFrames && std::ranges::all_of(gAutoLoads, [](const auto& load) { return load.IsReady(); }))
		if ((*gAutoLoadExitFrames)-- == 0)
			core::Application::Get()->RequestExit();

	if (BeginMainMenuBar())
	{
		if (BeginMenu("File"))
		{
			if (MenuItem("Open Model..."))
			{
				static const std::vector<FileFilter> kFilterList ={
					FileFilter{.name = "Models (Wavefront OBJ, glTF)", .spec = "obj,gltf,glb"}
				};
				InternalOpenFileDialogueAsync((resourcePath / "models").string(), kFilterList,
					[&rhi](std::string_view filePath, std::atomic_uint8_t& progressOut)
					{ LoadAndInstallModel(rhi, filePath, progressOut); });
			}
			if (MenuItem("Open Zip..."))
			{
				static const std::vector<FileFilter> kFilterList = {
					FileFilter{.name = "Zip archives", .spec = "zip"}
				};
				InternalOpenFileDialogueAsync((resourcePath / "models").string(), kFilterList,
					[&rhi](std::string_view filePath, std::atomic_uint8_t& progressOut)
					{ LoadAndInstallArchive(rhi, filePath, progressOut, ArchiveModels::kChoose); });
			}
			if (MenuItem("Open Image..."))
			{
				static const std::vector<FileFilter> kFilterList = {
					FileFilter{.name = "Image files", .spec = "jpg,jpeg,png,bmp,tga,gif,psd,hdr,pic,pnm"}
				};

				InternalOpenFileDialogueAsync((resourcePath / "images").string(), kFilterList,
					[&rhi](std::string_view filePath, std::atomic_uint8_t& progressOut)
					{ LoadAndInstallImage(rhi, filePath, progressOut); });
			}
			// if (MenuItem("Open Scene..."))
			// {
			// 	static const std::vector<FileFilter> filterList = {
			// 		FileFilter{.name = "Scene files", .spec = "gltf,glb"}
			// 	};

			// 	InternalOpenFileDialogueAsync((resourcePath / "scenes").string(), filterList, 
			// 		[&scene = Scene{}](std::string_view filePath, std::atomic_uint8_t& progress){ scene::LoadScene(scene, filePath, progress); });
			// }
			Separator();
			if (MenuItem("Exit", "CTRL+Q"))
				core::Application::Get()->RequestExit();

			ImGui::EndMenu();
		}
		if (BeginMenu("View"))
		{
			// if (MenuItem("Node Editor..."))
			// 	showNodeEditor = !showNodeEditor;
			if (BeginMenu("Layout"))
			{
				// the views' grid is owned by the draw thread (see Views::OnResizeGrid), so track the
				// requested one here. nothing else changes the grid, so the window's is current when this is initialized.
				static Extent2d gSplitScreenGrid{.width = myViews->GetGrid().x, .height = myViews->GetGrid().y};
				Extent2d& splitScreenGrid = gSplitScreenGrid;

				//static bool hasChanged = 
				bool selected1x1 = splitScreenGrid.width == 1 && splitScreenGrid.height == 1;
				bool selected1x2 = splitScreenGrid.width == 1 && splitScreenGrid.height == 2;
				bool selected2x1 = splitScreenGrid.width == 2 && splitScreenGrid.height == 1;
				bool selected2x2 = splitScreenGrid.width == 2 && splitScreenGrid.height == 2;
				bool anyChanged = false;

				if (MenuItem("1x1", "Ctrl+1", &selected1x1) && selected1x1)
				{
					splitScreenGrid.width = 1;
					splitScreenGrid.height = 1;
					anyChanged = true;
				}
				else if (MenuItem("1x2", "Ctrl+2", &selected1x2) && selected1x2)
				{
					splitScreenGrid.width = 1;
					splitScreenGrid.height = 2;
					anyChanged = true;
				}
				else if (MenuItem("2x1", "Ctrl+3", &selected2x1) && selected2x1)
				{
					splitScreenGrid.width = 2;
					splitScreenGrid.height = 1;
					anyChanged = true;
				}
				else if (MenuItem("2x2", "Ctrl+4", &selected2x2) && selected2x2)
				{
					splitScreenGrid.width = 2;
					splitScreenGrid.height = 2;
					anyChanged = true;
				}

				ImGui::EndMenu();

				if (anyChanged)
				{
					// also upload the new views: otherwise that only happens on the next input change
					auto [resizeTask, resizeFuture] = core::CreateTask(
						[this, grid = splitScreenGrid]
						{
							myViews->OnResizeGrid({grid.width, grid.height});
							myViews->UpdateBuffers();
						});
					rhi.drawCalls.enqueue(resizeTask);
				}
			}
			MenuItem("FPS", nullptr, &gShowFps);
			MenuItem("TPS", nullptr, &gShowTps);
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
			{
				if (MenuItem("Statistics..."))
					gShowStatistics = !gShowStatistics;
			}
#endif
			ImGui::EndMenu();
		}
		if (BeginMenu("About"))
		{
			if (MenuItem("Show IMGUI Demo..."))
				gShowDemoWindow = !gShowDemoWindow;
			Separator();
			if (MenuItem("About client..."))
				gShowAbout = !gShowAbout;
			ImGui::EndMenu();
		}

		EndMainMenuBar();
	}

	Render();

	// process texture creates/updates/destroys here rather than in the render thread: the snapshot below
	// outlives this frame, and imgui frees ImTextureData (e.g. when the font atlas grows) on the next NewFrame().
	gIMGUIRenderer->UpdateTextures(++gIMGUIFrameSequence);

	if (auto *data = GetDrawData())
	{
		auto& [snapshot, drawData, sequence] = gIMGUIFrames[gIMGUIWriteFrame];
		snapshot.SnapUsingSwap(data, &drawData, GetTime());
		sequence = gIMGUIFrameSequence;

		// detach the snapshot from imgui-owned texture data: resolve texture refs to their (already uploaded)
		// backend ids, and drop the texture list so RenderDrawData in the render thread doesn't touch it.
		for (ImDrawList* drawList : drawData.CmdLists)
			for (ImDrawCmd& drawCmd : drawList->CmdBuffer)
				drawCmd.TexRef = ImTextureRef(drawCmd.GetTexID());
		drawData.Textures = nullptr;

		// publish, and continue with the previous pending frame (whether or not the draw thread took it)
		gIMGUIWriteFrame = gIMGUIPendingFrame.exchange(gIMGUIWriteFrame | kIMGUIFrameFresh, std::memory_order_acq_rel) & ~kIMGUIFrameFresh;
	}
}

void WindowedApplication::RequestExit() noexcept
{
	Application::RequestExit();
	glfwPostEmptyEvent(); // thread safe
}

bool WindowedApplication::Main()
{
	using namespace windowedapplication;
	
	ZoneScopedN("WindowedApplication::Main");

	auto& rhi = GetRHI();

	core::TaskHandle mainCall;
	while (rhi.mainCalls.try_dequeue(mainCall))
	{
		GetExecutor().Call(mainCall);
	}

	return !IsExitRequested();
}

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

	if (!imguiIO.WantCaptureMouse && !imguiIO.WantCaptureKeyboard)
	{
		myViews->OnInputStateChanged(input);

		auto [updateTask, updateFuture] = core::CreateTask([this] { myViews->UpdateBuffers(); });
		GetRHI().drawCalls.enqueue(updateTask);
	}
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
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto& swapchain = window.GetSwapchain();
	auto& pipeline = rhi.GetPrimaryDevice().GetPipeline();
	auto& executor = GetExecutor();

	if (window.IsMinimized())
		return false;

	// e.g. a fullscreen switch can leave the swapchain at the previous size without a matching resize event
	if (swapchain.NeedsRecreate() && !RecreateWindowDependentObjects(rhi, window))
		return false;

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
		
		auto& renderImageSet = *device.GetResource<RenderImageSet>(gRenderImageSetUuids[newFrameIndex]);

		auto cmd = graphicsQueue.GetPool().Commands();

		//NOLINTBEGIN(bugprone-suspicious-stringview-data-usage)

		ZoneScopedN("WindowedApplication::Draw::submit");

		GPU_SCOPE_COLLECT(cmd, graphicsQueue);
		
		DrawMainPass(
			rhi,
			window,
			pipeline,
			graphicsQueue,
			cmd,
			newFrameIndex,
			graphics->timeline);
		{
			GPU_SCOPE(cmd, graphicsQueue, computeMain);

			renderImageSet.SetLoadOp(LoadOp::kLoad, 0);
			renderImageSet.SetLoadOp(LoadOp::kLoad, renderImageSet.GetAttachments().size() - 1, LoadOp::kClear);
			renderImageSet.SetStoreOp(StoreOp::kStore, 0);
			renderImageSet.SetStoreOp(StoreOp::kStore, renderImageSet.GetAttachments().size() - 1, StoreOp::kStore);
			renderImageSet.Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor, 0);
			renderImageSet.Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kDepth | ImageAspect::kStencil, renderImageSet.GetAttachments().size() - 1);

			swapchain.SetLoadOp(LoadOp::kClear, 0);
			swapchain.SetStoreOp(StoreOp::kStore, 0);
			swapchain.Transition(cmd, ImageLayout::kGeneral, ImageAspect::kColor, 0);

			pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kCompute);

			pipeline.SetDescriptorData(
				"gTextures",
				ImageBinding{
					.sampler={},
					.imageView=renderImageSet.GetAttachments()[0],
					.layout=renderImageSet.GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				newFrameIndex);

			pipeline.SetDescriptorData(
				"gRWTextures",
				ImageBinding{
					.sampler={},
					.imageView=swapchain.GetAttachments()[0],
					.layout=swapchain.GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				newFrameIndex);

			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES);
			pipeline.BindPipelineAuto(cmd);

			PushConstants pushConstants{.frameIndex = newFrameIndex};

			pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));

			// cover the whole swapchain image: ComputeMain has 16x16 threads per group, each copying a 16x16 pixel bucket
			constexpr uint32_t kComputePixelsPerGroup = 16U * 16U;
			auto dstExtent = swapchain.GetExtent();
			CommandEncoder(cmd).Dispatch(
				(dstExtent.width + kComputePixelsPerGroup - 1) / kComputePixelsPerGroup,
				(dstExtent.height + kComputePixelsPerGroup - 1) / kComputePixelsPerGroup,
				1U);
		}
		// {
		// 	GPU_SCOPE(cmd, graphicsQueue, copy);

		// 	renderImageSet.Transition(cmd, ImageLayout::kTransferSource, ImageAspect::kColor, 0);
		// 	window.Copy(
		// 		cmd,
		// 		renderImageSet,
		// 		{ImageAspect::kColor, 0, 0, 1},
		// 		0,
		// 		{ImageAspect::kColor, 0, 0, 1},
		// 		0);
		// }
		// {
		// 	GPU_SCOPE(cmd, graphicsQueue, blit);

		// 	renderImageSet.Transition(cmd, ImageLayout::kTransferSource, ImageAspect::kColor, 0);
		// 	window.Blit(
		// 		cmd,
		// 		renderImageSet,
		// 		{ImageAspect::kColor, 0, 0, 1},
		// 		0,
		// 		{ImageAspect::kColor, 0, 0, 1},
		// 		0,
		// 		Filter::kNearest);
		// }
		std::vector<core::TaskHandle> graphicsCallbacks;
		{
			GPU_SCOPE(cmd, graphicsQueue, imguiTextures);

			IMGUIPrepareFrame(cmd, graphicsCallbacks);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, imgui);

			swapchain.SetLoadOp(LoadOp::kLoad, 0);
			swapchain.SetStoreOp(StoreOp::kStore, 0);
			swapchain.Transition(cmd, ImageLayout::kColorAttachment, ImageAspect::kColor, 0);
			
			pipeline.SetRenderTarget(newFrame);
			
			swapchain.Begin(cmd, SubpassContents::kInline);
			IMGUIDraw(cmd);
			swapchain.End(cmd);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, Transition);
			
			swapchain.Transition(cmd, ImageLayout::kPresent, ImageAspect::kColor, 0);
		}

		cmd.End();
		//NOLINTEND(bugprone-suspicious-stringview-data-usage)

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
			.callbacks = std::move(graphicsCallbacks)});

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
	: Application(std::forward<std::string_view>(appName), std::forward<core::Environment>(env))
	, myRHI(std::make_unique<RHI>(RHIInitializationData{
		  .name = appName,
		  .createWindowFunc = createWindowFunc,
		  .descriptorPoolSizes = windowedapplication::DescriptorPoolSizes()}))
{
	using namespace windowedapplication;

	// the window the rhi created is the one we draw in
	SetCurrentWindow(GetRHI().GetWindows().front());

	auto& rhi = GetRHI();
	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto& pipeline = device.GetPipeline();

	myViews = std::make_unique<Views>(
		device, glm::uvec2(window.GetSwapchain().GetDesc().extent.width, window.GetSwapchain().GetDesc().extent.height));

	std::vector<core::TaskHandle> timelineCallbacks;

	constexpr uint32_t kBlackTextureWidth = 4;
	constexpr uint32_t kBlackTextureHeight = 4;
	constexpr uint32_t kBlackTextureSize = kBlackTextureWidth * kBlackTextureHeight * 4;
	
	auto blackTexture = device.CreateResource<Image>(
		ImageCreateDesc{
			device.CreateDeviceObjectCreateDesc("Black Texture"),
			{ImageMipLevelDesc{.extent = Extent2d{.width=kBlackTextureWidth, .height=kBlackTextureHeight}, .size = kBlackTextureSize, .offset = 0}},
			Format::kR8G8B8A8Unorm,
			ImageTiling::kLinear,
			ImageUsage::kSampled | ImageUsage::kTransferDestination,
			MemoryProperty::kDeviceLocal,
			ImageAspect::kColor,
			ImageLayout::kUndefined
		});
	auto blackTextureView = device.CreateResource<ImageView>(
		ImageViewCreateDesc{
			device.CreateDeviceObjectCreateDesc("Black Texture View"),
			*blackTexture,
			blackTexture->GetDesc().format,
			ImageAspect::kColor});
	gBlackTextureUuid = blackTexture->GetUuid();
	gBlackTextureViewUuid = blackTextureView->GetUuid();

	constexpr float kDefaultSamplerMaxAnisotropy = 16.0F;
	std::vector<SamplerDesc> samplerDescs{SamplerDesc{.maxAnisotropy = kDefaultSamplerMaxAnisotropy}};
	auto samplers = device.CreateResource<SamplerVector>(
		SamplerVectorCreateDesc{
			device.CreateDeviceObjectCreateDesc("Samplers"),
			std::move(samplerDescs)});
	gSamplersUuid = samplers->GetUuid();

	// initialize stuff on graphics queue
	static_assert(kDefaultSamplerId < SHADER_TYPES_GLOBAL_SAMPLER_COUNT);
	{
		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
		
		IMGUIInit(window, rhi, graphicsQueue, graphics->queues.Capacity(), myImGuiIniSettings);

		auto cmd = graphicsQueue.GetPool().Commands();

		blackTexture->Transition(cmd, ImageLayout::kTransferDestination);
		blackTexture->Clear(cmd, {.color = {0.0F, 0.0F, 0.0F, 1.0F}});
		blackTexture->Transition(cmd, ImageLayout::kShaderReadOnly);

		// white and untextured, until InstallModel and InstallImage fill them in
		std::vector<MaterialData> materialData(SHADER_TYPES_MATERIAL_COUNT);
		for (auto& material : materialData)
			std::ranges::fill(material.color, 1.0F);

		core::TaskCreateInfo<void> materialTransfersDone;
		auto materials = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("Materials"),
				SHADER_TYPES_MATERIAL_COUNT * sizeof(MaterialData),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible},
			materialData.data(),
			cmd,
			materialTransfersDone);
		gMaterialsUuid = materials->GetUuid();
		timelineCallbacks.emplace_back(materialTransfersDone.handle);

		constexpr uint32_t kDefaultModelInstanceId = 666;
		constexpr uint32_t kMatrix4x4ElementCount = 16;
		std::vector<ModelInstance> modelInstances(SHADER_TYPES_MODEL_INSTANCE_COUNT);
		static const auto kIdentityMatrix = glm::mat4x4(1.0);
		std::copy_n(&kIdentityMatrix[0][0], kMatrix4x4ElementCount, &modelInstances[kDefaultModelInstanceId].modelTransform[0][0]);
		auto modelTransform = glm::make_mat4(&modelInstances[kDefaultModelInstanceId].modelTransform[0][0]);
		auto inverseTransposeModelTransform = glm::transpose(glm::inverse(modelTransform));
		std::copy_n(&inverseTransposeModelTransform[0][0], kMatrix4x4ElementCount, &modelInstances[kDefaultModelInstanceId].inverseTransposeModelTransform[0][0]);

		core::TaskCreateInfo<void> modelTransfersDone;
		auto modelInstancesBuffer = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("ModelInstances"),
				SHADER_TYPES_MODEL_INSTANCE_COUNT * sizeof(ModelInstance),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible
			},
			modelInstances.data(),
			cmd,
			modelTransfersDone);
		gModelInstancesUuid = modelInstancesBuffer->GetUuid();
		timelineCallbacks.emplace_back(modelTransfersDone.handle);

		cmd.End();

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
			.waitSemaphores = {},
			.waitDstStageMasks = {},
			.waitSemaphoreValues = {},
			.signalSemaphores = {graphics->semaphore},
			.signalSemaphoreValues = {++graphics->timeline},
			.callbacks = std::move(timelineCallbacks)});

		graphicsSubmits |= graphicsQueue.Submit();
	}

	auto shaderIncludePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["RootPath"]) / "src/gfx/shaders";
	auto shaderIntermediatePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / ".slang.intermediate";

	ShaderLoader shaderLoader({shaderIncludePath}, {}, shaderIntermediatePath);

	auto shaderSourceFile = shaderIncludePath / "shaders.slang";

	const auto& [zPrepassShaderLayoutPairIt, zPrepassShaderLayoutWasInserted] = device.GetPipelineLayoutHandles().emplace(
		std::hash<std::string_view>{}("VertexZPrepass"),
		pipeline.CreateLayout(shaderLoader.Load(
			shaderSourceFile,
			{
				.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
				.entryPoints = {{"VertexZPrepass", SLANG_STAGE_VERTEX}},
				.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
			})));

	pipeline.BindLayoutAuto(zPrepassShaderLayoutPairIt->second, PipelineBindPoint::kGraphics);

	pipeline.SetDescriptorData(
		"gModelInstances",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gModelInstancesUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		pipeline.SetDescriptorData(
			"gViewData",
			BufferBinding{.buffer = myViews->GetBuffer(i), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}

	const auto& [mainShaderLayoutPairIt, mainShaderLayoutWasInserted] = device.GetPipelineLayoutHandles().emplace(
		std::hash<std::string_view>{}("Main"),
		pipeline.CreateLayout(shaderLoader.Load(
			shaderSourceFile,
			{
				.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
				.entryPoints = {
					{"VertexMain", SLANG_STAGE_VERTEX},
					{"FragmentMain", SLANG_STAGE_FRAGMENT},
					{"ComputeMain", SLANG_STAGE_COMPUTE},
				},
				.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
			})));

	pipeline.BindLayoutAuto(mainShaderLayoutPairIt->second, PipelineBindPoint::kGraphics);

	pipeline.SetDescriptorData(
		"gMaterialData",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gMaterialsUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_MATERIAL);

	pipeline.SetDescriptorData(
		"gModelInstances",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gModelInstancesUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

	pipeline.SetDescriptorData(
		"gSamplers",
		ImageBinding{.sampler = (*device.GetResource<SamplerVector>(gSamplersUuid))[0]},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
		kDefaultSamplerId);

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		pipeline.SetDescriptorData(
			"gViewData",
			BufferBinding{.buffer = myViews->GetBuffer(i), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}

	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kCompute);

	CreateWindowDependentObjects(rhi);
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

	ShutdownImgui();

	// what holds gpu objects, before the rhi they belong to
	gModel.reset();
	myViews.reset();
	myRHI.reset();
}

void WindowedApplication::OnResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace windowedapplication;

	std::unique_lock lock(gDrawMutex);

	ZoneScopedN("WindowedApplication::OnResizeFramebuffer");

	auto& rhi = GetRHI();
	auto& rhiWindow = rhi.GetWindow(window);

	// minimizing reports 0x0: keep the swapchain, and have Draw skip frames until the window is restored
	rhiWindow.SetMinimized(width <= 0 || height <= 0);
	if (rhiWindow.IsMinimized())
		return;

	// the swapchain is sized after the surface (which already has this size) rather than width/height
	RecreateWindowDependentObjects(rhi, rhiWindow);
}

WindowState* WindowedApplication::GetWindowState(WindowHandle window)
{
	return &GetRHI().GetWindow(window).GetState();
}

uint32_t WindowedApplication::GetWindowCount() const noexcept
{
	return GetRHI().GetWindows().size();
}

WindowHandle WindowedApplication::GetWindow(uint32_t index) const noexcept
{
	return *std::next(GetRHI().GetWindows().begin(), index);
}

} // namespace gfx

WindowHandle GetCurrentWindow(void)
{
	return gfx::gCurrentWindow.value_or(kInvalidWindowHandle);
}

void SetCurrentWindow(WindowHandle window)
{
	if (!gfx::gCurrentWindow.has_value())
		gfx::gCurrentWindow = window;
}

void ResizeFramebuffer(WindowHandle window, int width, int height)
{
	if (auto app = std::static_pointer_cast<gfx::WindowedApplication>(core::Application::Get()); app)
		app->OnResizeFramebuffer(window, width, height);
}

WindowState* GetWindowState(WindowHandle window)
{
	if (auto app = std::static_pointer_cast<gfx::WindowedApplication>(core::Application::Get()); app)
		return app->GetWindowState(window);

	return nullptr;
}
