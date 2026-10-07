#include <core/task.h>
#include <gfx/capi.h>
#include <gfx/gpu.h>
#include <gfx/imgui_extra.h>
#include <gfx/meshimport.h>
#include <gfx/model.h>
#include <gfx/shaderloader.h>
#include <gfx/shaders/capi.h>
#include <gfx/texture.h>
#include <gfx/windowedapplication.h>
#include <gfx/ziparchive.h>
#include <rhi/capi.h>
#include <rhi/renderimageset.h>

#include <uuid.h>
#include <xxhash.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <bit>
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
#include <utility>

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
// the environment (see InstallEnvironment): gEnvironment's buffer, and the prefiltered panorama in
// SHADER_TYPES_ENVIRONMENT_TEXTURE (and for sheen, SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE), nil until one is
// installed. its name, for the menu
static uuids::uuid gEnvironmentUuid;
static uuids::uuid gEnvironmentImageUuid;
static uuids::uuid gEnvironmentViewUuid;
static uuids::uuid gEnvironmentSheenImageUuid;
static uuids::uuid gEnvironmentSheenViewUuid;
static core::ConcurrentAccess<std::string> gEnvironmentName;
// the order independent transparency's per pixel lists (see OitNode): their heads (a node index per pixel of the render
// target, gOitWidth wide), nodes (gOitNodeCapacity) and counter, sized with the render target (see
// CreateWindowDependentObjects), and cleared before each frame's main pass
static uuids::uuid gOitHeadsUuid;
static uuids::uuid gOitNodesUuid;
static uuids::uuid gOitCounterUuid;
static uint32_t gOitWidth = 0;
static uint32_t gOitNodeCapacity = 0;
// the opaque scene with mips, which transmissive materials refract (SHADER_TYPES_TRANSMISSION_TEXTURE), sized with the
// render target
static uuids::uuid gTransmissionImageUuid;
static uuids::uuid gTransmissionViewUuid;

// the main pass draws in two phases when the model has transmissive materials: the opaque ones that aren't, then (after
// the color is copied to the transmission texture) the transmissive and blended ones. else the first draws everything.
enum class MainPassPhase : uint8_t
{
	kOpaque,
	kTransmissive,
};

// the layers a material draws: those on at rest, and those an animation turns on (KHR_animation_pointer: a factor that
// animates up from 0)
struct MaterialLayers
{
	bool clearcoat = false;
	bool sheen = false;
	bool transmission = false;
	bool anisotropy = false;
	bool iridescence = false;
	bool diffuseTransmission = false;
};

[[nodiscard]] static MaterialLayers LayersOf(const ModelDesc& desc, size_t materialIndex)
{
	const auto& material = desc.materials[materialIndex];
	MaterialLayers layers{
		.clearcoat = material.clearcoat > 0.0F,
		.sheen = std::ranges::any_of(material.sheenColor, [](float c) { return c > 0.0F; }),
		.transmission = material.transmission > 0.0F,
		.anisotropy = material.anisotropy > 0.0F,
		.iridescence = material.iridescence > 0.0F,
		.diffuseTransmission = material.diffuseTransmission > 0.0F};
	for (const auto& target : desc.animation.pointerTargets)
	{
		if (target.kind != ScenePointerTarget::Kind::kMaterial || target.index != materialIndex)
			continue;
		switch (MaterialProperty{target.property})
		{
		case MaterialProperty::kClearcoat: layers.clearcoat = true; break;
		case MaterialProperty::kSheenColor: layers.sheen = true; break;
		case MaterialProperty::kTransmission: layers.transmission = true; break;
		case MaterialProperty::kAnisotropy: layers.anisotropy = true; break;
		case MaterialProperty::kIridescence: layers.iridescence = true; break;
		case MaterialProperty::kDiffuseTransmission: layers.diffuseTransmission = true; break;
		default: break;
		}
	}
	return layers;
}

[[nodiscard]] static bool HasTransmission(const Model& model)
{
	const auto& desc = model.GetDesc();
	for (size_t materialIt = 0; materialIt < desc.materials.size(); materialIt++)
		if (!desc.materials[materialIt].blend && LayersOf(desc, materialIt).transmission)
			return true;
	return false;
}
static uuids::uuid gSamplersUuid;
static uuids::uuid gModelSamplersUuid; // the loaded model's samplers, see InstallModel. nil until one is loaded
static uuids::uuid gMaterialsUuid;
static uuids::uuid gTextureViewsUuid;
static uuids::uuid gModelInstancesUuid;
// bound as gSkinVertices, gJointMatrices, gMorphDeltas and gMorphWeights while the installed model has none (a model
// that doesn't move, isn't skinned or has no animated morph targets): one zero skin vertex, one identity joint matrix,
// one zero morph delta and one zero weight
static uuids::uuid gDefaultSkinVerticesUuid;
static uuids::uuid gDefaultJointsUuid;
static uuids::uuid gDefaultMorphDeltasUuid;
static uuids::uuid gDefaultMorphWeightsUuid;

// which of the installed model's animations plays (see Model::Animate), and its clock. the ui thread reads and changes
// it, the draw thread advances it.
struct AnimationState
{
	static constexpr double kCrossfade = 0.3; // seconds a switch of animation fades over

	std::vector<std::string> names;
	std::optional<size_t> selected; // nullopt: the rest pose
	bool playing = true;
	double time = 0.0; // seconds
	std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
	// what is fading out after a switch (and still playing), and for how long it has
	std::optional<size_t> previous;
	double previousTime = 0.0;
	double fade = kCrossfade;

	// switches to an animation (or the rest pose), from its start, crossfading from the current one
	void Select(std::optional<size_t> animation)
	{
		if (animation == selected)
			return;
		previous = selected;
		previousTime = time;
		fade = 0.0;
		selected = animation;
		time = 0.0;
	}
};
static core::ConcurrentAccess<AnimationState> gAnimation;

// the installed model's file and its scenes (a gltf file's, see ModelDesc::scenes), for View > Scene. the draw thread
// sets it, the ui thread reads it.
struct SceneState
{
	std::string filePath;
	std::vector<std::string> names;
	uint32_t current = 0;
};
static core::ConcurrentAccess<SceneState> gScenes;
// gLights (SHADER_TYPES_LIGHT_COUNT of them): the installed model's, or the default light. draw thread.
static uuids::uuid gLightsUuid;
static uint32_t gLightCount = 0;
// the final image is scaled by 2^gExposureStops before tonemapping (with auto exposure, on top of what it picks). set
// from the ui, read by the draw thread.
static std::atomic<float> gExposureStops = 0.0F;
// auto exposure: on or off (from the ui), and what it picked, in stops (the draw thread's: see UpdateAutoExposure)
static std::atomic<bool> gAutoExposure = false;
static std::atomic<float> gAutoExposureStops = 0.0F; // written by the draw thread, shown by the ui
static std::optional<std::chrono::steady_clock::time_point> gAutoExposureLast;
static uuids::uuid gExposureHistogramUuid; // gExposureHistogram: SHADER_TYPES_EXPOSURE_BINS per frame, host visible
// the tonemapper (TONEMAPPER_*). set from the ui, read by the draw thread.
static std::atomic<uint32_t> gTonemapper = TONEMAPPER_PBR_NEUTRAL;
// the environment's intensity (radiance scale) and rotation about +y (degrees), and whether it is drawn as the backdrop.
// set from the ui, read by the draw thread (see PushConstants::environmentIntensity)
static std::atomic<float> gEnvironmentIntensity = 1.0F;
static std::atomic<float> gEnvironmentRotationDegrees = 0.0F;
static std::atomic<bool> gEnvironmentBackdrop = true;

// the push constants every draw (and ComputeMain) of a frame shares
static PushConstants FramePushConstants(uint16_t frameIndex)
{
	return PushConstants{
		.frameIndex = frameIndex,
		.lightCount = gLightCount,
		.exposure = std::exp2(
			gExposureStops.load(std::memory_order_relaxed) +
			(gAutoExposure.load(std::memory_order_relaxed) ? gAutoExposureStops.load(std::memory_order_relaxed) : 0.0F)),
		.environmentIntensity = gEnvironmentIntensity.load(std::memory_order_relaxed),
		.environmentRotation = glm::radians(gEnvironmentRotationDegrees.load(std::memory_order_relaxed)),
		.viewCount = std::min<uint32_t>(App().GetViews().GetGrid().x * App().GetViews().GetGrid().y, SHADER_TYPES_VIEW_COUNT),
		.environmentBackdrop = gEnvironmentBackdrop.load(std::memory_order_relaxed) ? 1U : 0U,
		.framebufferWidth = gOitWidth,
		.oitNodeCapacity = gOitNodeCapacity,
		.tonemapper = gTonemapper.load(std::memory_order_relaxed)};
}

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

// the main render target's color attachments (see FragmentOutput in the shaders), then its depth
static constexpr uint32_t kColorAttachment = 0;
static constexpr uint32_t kDepthAttachment = 1;
// opaque draws write the color; blended ones nothing (with FragmentTransparent, the layout's second fragment entry point,
// they go in the per pixel lists of the order independent transparency, see OitNode), so they needn't be sorted
static constexpr std::array kOpaqueBlend{BlendMode::kOpaque, BlendMode::kNone, BlendMode::kNone, BlendMode::kNone};
static constexpr std::array kTransparentBlend{BlendMode::kNone, BlendMode::kNone, BlendMode::kNone, BlendMode::kNone};
static constexpr uint8_t kTransparentFragmentShader = 1;
static_assert(kOpaqueBlend.size() == kMaxColorAttachments);

// gTextures slots 0 to SHADER_TYPES_FRAME_COUNT - 1 hold the frames' render targets (for ComputeMain), and
// SHADER_TYPES_ENVIRONMENT_TEXTURE and SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE the environment's (and
// SHADER_TYPES_TRANSMISSION_TEXTURE the opaque scene). material 0 is the default material, for models (or parts of them) without one: it samples this slot, which opening an image replaces.
static constexpr uint32_t kMaterialTextureId = 15;
// the loaded model's materials are 1 and up, and their textures are in the slots from here up
static constexpr uint32_t kModelTextureFirstSlot = 16;
static constexpr uint32_t kModelTextureMaxCount = SHADER_TYPES_GLOBAL_TEXTURE_COUNT - kModelTextureFirstSlot;
static constexpr uint32_t kModelMaterialMaxCount = SHADER_TYPES_MATERIAL_COUNT - 1;
static constexpr uint32_t kDefaultSamplerId = 2;
// the sampler slots a model's samplers go in: all but the default's and the clamping one's
// (SHADER_TYPES_CLAMP_SAMPLER)
static_assert(kDefaultSamplerId != SHADER_TYPES_CLAMP_SAMPLER);
static constexpr auto kModelSamplerSlots = []
{
	std::array<uint32_t, SHADER_TYPES_GLOBAL_SAMPLER_COUNT - 2> slots{};
	for (uint32_t slot = 0, slotIt = 0; slot < SHADER_TYPES_GLOBAL_SAMPLER_COUNT; slot++)
		if (slot != kDefaultSamplerId && slot != SHADER_TYPES_CLAMP_SAMPLER)
			slots[slotIt++] = slot;
	return slots;
}();
static size_t gModelSamplerCount = 0; // how many of kModelSamplerSlots the loaded model uses
static_assert(
	kMaterialTextureId >= SHADER_TYPES_FRAME_COUNT && kMaterialTextureId != SHADER_TYPES_ENVIRONMENT_TEXTURE &&
	kMaterialTextureId != SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE && kMaterialTextureId != SHADER_TYPES_TRANSMISSION_TEXTURE &&
	SHADER_TYPES_ENVIRONMENT_TEXTURE >= SHADER_TYPES_FRAME_COUNT && SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE >= SHADER_TYPES_FRAME_COUNT &&
	kMaterialTextureId < kModelTextureFirstSlot);

// the material slot drawn for a submesh of the loaded model
static uint32_t ModelMaterialSlot(int32_t material)
{
	return material >= 0 && std::cmp_less(material, kModelMaterialMaxCount) ? static_cast<uint32_t>(material) + 1 : 0;
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
		.callbacks = std::move(callbacks),});

	graphicsSubmits |= graphicsQueue.Submit();
}

// writes data to buffer at offset, in a submission on the graphics queue that is ordered after all graphics work
// submitted so far, and before all that follows. call on the draw thread.
static void UpdateBufferOnGraphics(
	QueueTimelineContextData& graphics, const Buffer& buffer, uint64_t offset, std::span<const std::byte> data)
{
	if (data.empty())
		return;

	auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();

	auto cmd = graphicsQueue.GetPool().Commands();
	{
		GPU_SCOPE(cmd, graphicsQueue, UpdateBuffer); //NOLINT(bugprone-suspicious-stringview-data-usage)

		CommandEncoder encoder(cmd);

		// after the frames in flight have read the old data
		encoder.Barrier(PipelineStage::kAllCommands, Access::kShaderRead, PipelineStage::kTransfer, Access::kTransferWrite);

		// UpdateBuffer takes 64 kB at most
		constexpr size_t kMaxUpdateSize = 65536;
		for (size_t chunk = 0; chunk < data.size(); chunk += kMaxUpdateSize)
			encoder.UpdateBuffer(buffer, offset + chunk, data.subspan(chunk, std::min(kMaxUpdateSize, data.size() - chunk)));

		encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kAllCommands, Access::kShaderRead);
	}
	cmd.End();

	graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {PipelineStage::kAllCommands},
		.waitSemaphoreValues = {graphics.timeline},
		.signalSemaphores = {graphics.semaphore},
		.signalSemaphoreValues = {++graphics.timeline},});

	graphicsSubmits |= graphicsQueue.Submit();
}

// writes materials, starting at slot first (see UpdateBufferOnGraphics). call on the draw thread.
static void UpdateMaterials(
	RHI& rhi, QueueTimelineContextData& graphics, uint32_t first, std::span<const MaterialData> materials)
{
	ENSURE(first + materials.size() <= SHADER_TYPES_MATERIAL_COUNT);

	UpdateBufferOnGraphics(
		graphics, *rhi.GetPrimaryDevice().GetResource<Buffer>(gMaterialsUuid), first * sizeof(MaterialData), std::as_bytes(materials));
}

// writes a model's lights to gLights, or the default light if it has none: a directional light from above that, with
// the ambient light in the shader, lights matte surfaces as before there were lights. call on the draw thread.
static void UpdateLights(RHI& rhi, QueueTimelineContextData& graphics, std::span<const SceneLight> sceneLights)
{
	static const SceneLight kDefaultLight{
		.name = "default",
		.direction = {-0.2638F, -0.8794F, -0.4397F}, // -normalize(0.3, 1, 0.5)
		.intensity = 2.2F,}; // lux: 0.7 pi, the diffuse light of a white surface facing it (0.7) times pi
	if (sceneLights.empty())
		sceneLights = std::span(&kDefaultLight, 1);
	if (sceneLights.size() > SHADER_TYPES_LIGHT_COUNT)
	{
		std::println(stderr, "{} lights, only the first {} are used", sceneLights.size(), SHADER_TYPES_LIGHT_COUNT);
		sceneLights = sceneLights.first(SHADER_TYPES_LIGHT_COUNT);
	}

	std::vector<LightData> lights(sceneLights.size());
	for (size_t lightIt = 0; lightIt < lights.size(); lightIt++)
	{
		const auto& sceneLight = sceneLights[lightIt];
		auto& light = lights[lightIt];
		std::ranges::copy(sceneLight.position, light.positionRange);
		light.positionRange[3] = sceneLight.range;
		std::ranges::copy(sceneLight.direction, light.direction);
		for (size_t channel = 0; channel < 3; channel++)
			light.intensity[channel] = sceneLight.color[channel] * sceneLight.intensity;
		light.type = sceneLight.type == SceneLight::Type::kPoint  ? LIGHT_TYPE_POINT
				   : sceneLight.type == SceneLight::Type::kSpot ? LIGHT_TYPE_SPOT
																  : LIGHT_TYPE_DIRECTIONAL;
		// KHR_lights_punctual's cone attenuation
		auto cosOuter = std::cos(sceneLight.outerConeAngle);
		light.spotScale = 1.0F / std::max(0.001F, std::cos(sceneLight.innerConeAngle) - cosOuter);
		light.spotOffset = -cosOuter * light.spotScale;
	}

	UpdateBufferOnGraphics(graphics, *rhi.GetPrimaryDevice().GetResource<Buffer>(gLightsUuid), 0, std::as_bytes(std::span(lights)));
	gLightCount = static_cast<uint32_t>(lights.size());
}

// writes texture views, starting at slot first (see UpdateBufferOnGraphics). call on the draw thread.
static void UpdateTextureViews(
	RHI& rhi, QueueTimelineContextData& graphics, uint32_t first, std::span<const TextureView> views)
{
	ENSURE(first + views.size() <= SHADER_TYPES_TEXTURE_VIEW_COUNT);

	UpdateBufferOnGraphics(
		graphics, *rhi.GetPrimaryDevice().GetResource<Buffer>(gTextureViewsUuid), first * sizeof(TextureView), std::as_bytes(views));
}

// what makes texture views the same, to share them: the transform by its bits, so that the key hashes as bytes
struct TextureViewKey
{
	uint32_t textureSlot = 0;
	uint32_t samplerSlot = 0;
	uint32_t texCoord = 0;
	std::array<uint32_t, 6> transform{};
	// 1 + the TextureRef::animatedTransform of a texture whose transform animates (its view is its own), else 0
	uint32_t animated = 0;
	uint32_t flags = 0; // TextureView::flags

	[[nodiscard]] bool operator==(const TextureViewKey&) const = default;
};
static_assert(std::has_unique_object_representations_v<TextureViewKey>, "TextureViewKey is hashed as bytes");

struct TextureViewKeyHash
{
	using is_avalanching = void; //NOLINT(readability-identifier-naming)

	[[nodiscard]] uint64_t operator()(const TextureViewKey& key) const noexcept { return XXH3_64bits(&key, sizeof(key)); }
};

// a view of a texture slot with a sampler slot, as a TextureRef samples it
[[nodiscard]] static TextureView MakeTextureView(uint32_t textureSlot, uint32_t samplerSlot, const TextureRef& ref, uint32_t flags = 0)
{
	const auto& tRef = ref.transform;
	return TextureView{
		.uTransform = {tRef[0], tRef[1], tRef[2], 0.0F},
		.vTransform = {tRef[3], tRef[4], tRef[5], 0.0F},
		.textureId = textureSlot,
		.samplerId = samplerSlot,
		.texCoordSet = ref.texCoord,
		.flags = flags,};
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
		.waitSemaphoreValues = {graphics.timeline},};
	std::vector<const Semaphore*> waitSemaphores{&graphics.semaphore};
	auto waitFor = [&](const Upload& upload)
	{
		ENSURE(upload.semaphore != nullptr);
		auto semaIt = std::ranges::find(waitSemaphores, upload.semaphore);
		auto index = static_cast<size_t>(semaIt - waitSemaphores.begin());
		if (semaIt == waitSemaphores.end())
		{
			waitSemaphores.push_back(upload.semaphore);
			syncInfo.waitSemaphores.emplace_back(*upload.semaphore);
			syncInfo.waitDstStageMasks.emplace_back(PipelineStage::kAllCommands);
			syncInfo.waitSemaphoreValues.emplace_back(upload.value);
		}
		else if (index > 0)
		{
			syncInfo.waitSemaphoreValues[index] = std::max(syncInfo.waitSemaphoreValues[index], upload.value);
		}
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
	Texture emissive;
	Texture occlusion;
	Texture metallicRoughness;
	Texture specular;
	Texture specularColor;
	Texture clearcoat;
	Texture clearcoatRoughness;
	Texture clearcoatNormal;
	Texture sheenColor;
	Texture sheenRoughness;
	Texture transmission;
	Texture thickness;
	Texture anisotropy;
	Texture iridescence;
	Texture iridescenceThickness;
	Texture diffuseTransmission;
	Texture diffuseTransmissionColor;
};

// the installed model's materials as uploaded (gMaterialData from 1), and its texture views whose transforms animate
// (KHR_animation_pointer), which ApplyPointerValues patches. draw thread only.
struct AnimatedTextureView
{
	uint32_t view = 0; // in gTextureViews
	uint32_t target = 0; // its offset's ScenePointerTarget (see TextureRef::animatedTransform)
	TextureView data; // as uploaded
};
static std::vector<MaterialData> gModelMaterialData;
static std::vector<AnimatedTextureView> gAnimatedTextureViews;
static std::vector<SceneLight> gAnimatedLights; // the lights as last uploaded, if some follow nodes

// a material value a KHR_animation_pointer channel sets (see ScenePointerTarget::Kind::kMaterial), into its MaterialData
static void ApplyMaterialProperty(MaterialData& data, const ModelMaterial& desc, MaterialProperty property, std::span<const float> v, float scale)
{
	auto copy = [&v](float* out, size_t count) { std::copy_n(v.begin(), std::min(count, v.size()), out); };
	if (v.empty())
		return;
	switch (property)
	{
	case MaterialProperty::kBaseColor: copy(data.color, 4); break;
	case MaterialProperty::kEmissive:
		for (size_t channel = 0; channel < std::min<size_t>(3, v.size()); channel++)
			data.emissive[channel] = v[channel] * scale;
		break;
	case MaterialProperty::kMetallic:
		if (!desc.specularGlossiness)
			data.metallic = v[0];
		break;
	case MaterialProperty::kRoughness:
		if (!desc.specularGlossiness)
			data.roughness = v[0];
		break;
	case MaterialProperty::kAlphaCutoff:
		if (desc.alphaCutoff > 0.0F) // only masked materials test it
			data.alphaCutoff = v[0];
		break;
	case MaterialProperty::kNormalScale: data.normalScale = v[0]; break;
	case MaterialProperty::kOcclusionStrength: data.emissive[3] = v[0]; break;
	case MaterialProperty::kSpecular: data.specular = v[0]; break;
	case MaterialProperty::kSpecularColor: copy(data.specularColor, 3); break;
	case MaterialProperty::kIor: data.specularColor[3] = v[0]; break;
	case MaterialProperty::kClearcoat: data.clearcoat[0] = v[0]; break;
	case MaterialProperty::kClearcoatRoughness: data.clearcoat[1] = v[0]; break;
	case MaterialProperty::kClearcoatNormalScale: data.clearcoat[2] = v[0]; break;
	case MaterialProperty::kSheenColor: copy(data.sheen, 3); break;
	case MaterialProperty::kSheenRoughness: data.sheen[3] = v[0]; break;
	case MaterialProperty::kTransmission: data.transmission[0] = v[0]; break;
	case MaterialProperty::kThickness: data.transmission[1] = v[0]; break;
	case MaterialProperty::kAttenuationDistance: data.transmission[2] = v[0]; break;
	case MaterialProperty::kDispersion: data.transmission[3] = v[0]; break;
	case MaterialProperty::kAttenuationColor: copy(data.attenuationColor, 3); break;
	case MaterialProperty::kAnisotropy: data.anisotropy[0] = v[0]; break;
	case MaterialProperty::kIridescence: data.iridescence[0] = v[0]; break;
	case MaterialProperty::kIridescenceIor: data.iridescence[1] = v[0]; break;
	case MaterialProperty::kIridescenceThicknessMin: data.iridescence[2] = v[0]; break;
	case MaterialProperty::kIridescenceThicknessMax: data.iridescence[3] = v[0]; break;
	case MaterialProperty::kDiffuseTransmission: data.diffuseTransmission[3] = v[0]; break;
	case MaterialProperty::kDiffuseTransmissionColor: copy(data.diffuseTransmission, 3); break;
	case MaterialProperty::kAnisotropyRotation:
		data.anisotropy[1] = std::cos(v[0]);
		data.anisotropy[2] = std::sin(v[0]);
		break;
	case MaterialProperty::kEmissiveStrength: break; // with kEmissive
	}
}

// a material's defaults: white, untextured, rough, dielectric, with the default specular (ior 1.5, white)
[[nodiscard]] static MaterialData DefaultMaterialData()
{
	MaterialData material{};
	std::ranges::fill(material.color, 1.0F);
	material.specularColor[0] = material.specularColor[1] = material.specularColor[2] = 1.0F;
	material.specularColor[3] = 1.5F;
	material.roughness = 1.0F;
	material.specular = 1.0F;
	return material;
}

// auto exposure: what a frame's histogram (gExposureHistogram, read once the frame's previous use is done) says the
// scene's luminance is, the mean log2 luminance of the pixels between the 50th and 95th percentile (the darkest and the
// brightest left out), toward which the exposure moves, a little each frame (by elapsed time), so that it is
// kAutoExposureKey: the default scene (the procedural sky) keeps the exposure it had before. call on the draw thread.
static void UpdateAutoExposure(RHI& rhi, uint32_t frameIndex)
{
	constexpr double kAutoExposureKey = 0.3;
	constexpr double kAdaptationSpeed = 2.0; // per second, of what is left to the target, as an exponential decay

	auto now = std::chrono::steady_clock::now();
	auto elapsed = gAutoExposureLast ? std::chrono::duration<double>(now - *gAutoExposureLast).count() : 0.0;
	gAutoExposureLast = now;

	auto& histogram = *rhi.GetPrimaryDevice().GetResource<Buffer>(gExposureHistogramUuid);
	auto memory = histogram.Map();
	std::array<uint32_t, SHADER_TYPES_EXPOSURE_BINS> bins{};
	std::memcpy(bins.data(), memory.data() + (static_cast<size_t>(frameIndex) * sizeof(bins)), sizeof(bins));
	histogram.Unmap();

	uint64_t total = std::accumulate(bins.begin(), bins.end(), uint64_t{0});
	if (total == 0 || !gAutoExposure.load(std::memory_order_relaxed))
		return;
	auto low = static_cast<double>(total) * 0.5;
	auto high = static_cast<double>(total) * 0.95;
	double counted = 0.0;
	double sum = 0.0;
	double weight = 0.0;
	for (size_t binIt = 0; binIt < bins.size(); binIt++)
	{
		// the part of the bin between the percentiles
		auto from = std::max(counted, low);
		auto to = std::min(counted + bins[binIt], high);
		counted += bins[binIt];
		if (to <= from)
			continue;
		auto log2Luminance = SHADER_TYPES_EXPOSURE_LOG2_MIN +
							 ((static_cast<double>(binIt) + 0.5) / SHADER_TYPES_EXPOSURE_BINS * (SHADER_TYPES_EXPOSURE_LOG2_MAX - SHADER_TYPES_EXPOSURE_LOG2_MIN));
		sum += log2Luminance * (to - from);
		weight += to - from;
	}
	if (weight <= 0.0)
		return;
	auto target = static_cast<float>(std::log2(kAutoExposureKey) - (sum / weight));
	// the first measure is taken as it is
	auto stops = gAutoExposureStops.load(std::memory_order_relaxed);
	if (elapsed <= 0.0 || elapsed > 1.0)
		stops = target;
	else
		stops += (target - stops) * static_cast<float>(1.0 - std::exp(-elapsed * kAdaptationSpeed));
	gAutoExposureStops.store(stops, std::memory_order_relaxed);
}

// what KHR_animation_pointer channels (and lights that follow nodes) set, as the model's last Animate left it: patches
// and uploads the materials, texture views and lights that changed. call on the draw thread.
static void ApplyPointerValues(RHI& rhi, QueueTimelineContextData& graphics, const Model& model)
{
	const auto& animation = model.GetDesc().animation;
	if (animation.pointerTargets.empty() && animation.lightLinks.empty())
		return;
	auto values = model.GetPointerValues();
	auto valuesOf = [&values](const ScenePointerTarget& target)
	{
		return target.valueBase + target.valueCount <= values.size() ? values.subspan(target.valueBase, target.valueCount)
																	 : std::span<const float>{};
	};

	// the materials, uploaded as one range from the first that changed to the last
	std::optional<size_t> first;
	size_t last = 0;
	for (size_t targetIt = 0; targetIt < animation.pointerTargets.size(); targetIt++)
	{
		const auto& target = animation.pointerTargets[targetIt];
		if (target.kind != ScenePointerTarget::Kind::kMaterial || target.index >= gModelMaterialData.size() ||
			target.index >= model.GetDesc().materials.size())
			continue;
		// the emissive factor times its strength, the target after it (see MaterialProperty::kEmissive)
		auto property = MaterialProperty{target.property};
		if (property == MaterialProperty::kEmissiveStrength)
			continue;
		float scale = 1.0F;
		if (property == MaterialProperty::kEmissive && targetIt + 1 < animation.pointerTargets.size())
			if (auto strength = valuesOf(animation.pointerTargets[targetIt + 1]); !strength.empty())
				scale = strength[0];
		auto& data = gModelMaterialData[target.index];
		auto before = data;
		ApplyMaterialProperty(data, model.GetDesc().materials[target.index], property, valuesOf(target), scale);
		if (std::memcmp(&before, &data, sizeof(MaterialData)) != 0)
		{
			first = std::min<size_t>(first.value_or(target.index), target.index);
			last = std::max<size_t>(last, target.index);
		}
	}
	if (first)
		UpdateMaterials(rhi, graphics, static_cast<uint32_t>(1 + *first), std::span(gModelMaterialData).subspan(*first, last - *first + 1));

	// the texture transforms (offset, rotation and scale, the three targets from the offset's)
	for (auto& animated : gAnimatedTextureViews)
	{
		if (animated.target + 2 >= animation.pointerTargets.size())
			continue;
		auto offset = valuesOf(animation.pointerTargets[animated.target]);
		auto rotation = valuesOf(animation.pointerTargets[animated.target + 1]);
		auto scale = valuesOf(animation.pointerTargets[animated.target + 2]);
		if (offset.size() < 2 || rotation.empty() || scale.size() < 2)
			continue;
		auto transform = TextureTransform({offset[0], offset[1]}, rotation[0], {scale[0], scale[1]});
		auto view = animated.data;
		std::ranges::copy(std::span(transform).first(3), view.uTransform);
		std::ranges::copy(std::span(transform).last(3), view.vTransform);
		if (std::memcmp(&view, &animated.data, sizeof(TextureView)) != 0)
		{
			animated.data = view;
			UpdateTextureViews(rhi, graphics, animated.view, std::span(&view, 1));
		}
	}

	// the lights that follow nodes (and their visibility), and whose values animate
	if (model.AnimatesLights())
	{
		auto lights = model.GetLights();
		auto same = [](const SceneLight& a, const SceneLight& b)
		{
			return a.position == b.position && a.direction == b.direction && a.intensity == b.intensity && a.color == b.color &&
				   a.range == b.range && a.innerConeAngle == b.innerConeAngle && a.outerConeAngle == b.outerConeAngle;
		};
		if (!std::ranges::equal(lights, gAnimatedLights, same))
		{
			gAnimatedLights.assign(lights.begin(), lights.end());
			UpdateLights(rhi, graphics, lights);
		}
	}

	// the views, when they look through a camera that animates
	if (model.AnimatesCameras())
		App().GetViews().UpdateSceneCameras(model.GetCameras());
}

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
	for (const auto* buffer : model->GetUploadedBuffers())
		uploads.buffers.emplace_back(buffer, model->GetUpload());
	for (const auto& material : textures)
		for (const auto* texture :
			 {&material.diffuse, &material.alpha, &material.normal, &material.emissive, &material.occlusion, &material.metallicRoughness,
			  &material.specular, &material.specularColor, &material.clearcoat, &material.clearcoatRoughness, &material.clearcoatNormal,
			  &material.sheenColor, &material.sheenRoughness, &material.transmission, &material.thickness,
			  &material.anisotropy, &material.iridescence, &material.iridescenceThickness,
			  &material.diffuseTransmission, &material.diffuseTransmissionColor})
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
			const auto& [image, view, upload, normalYUp] = texture;
			if (!image)
				return std::nullopt;

			if (auto slotIt = textureSlots.find(image.get()); slotIt != textureSlots.end())
				return slotIt->second;

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

		const auto& descMaterials = model->GetDesc().materials;

		// each distinct sampler once: the default in its slot, the others in the rest (see TextureRef::sampler)
		std::vector<SamplerDesc> samplerDescs;
		auto samplerSlotOf = [&samplerDescs, warned = false](const SamplerDesc& desc) mutable -> uint32_t
		{
			static const SamplerDesc kDefault = TextureRef{}.sampler;
			if (desc == kDefault)
				return kDefaultSamplerId;
			auto samplerDescIt = std::ranges::find(samplerDescs, desc);
			if (samplerDescIt == samplerDescs.end())
			{
				if (samplerDescs.size() == kModelSamplerSlots.size())
				{
					if (!std::exchange(warned, true))
						std::println(stderr, "more than {} different samplers, the rest use the default", kModelSamplerSlots.size());
					return kDefaultSamplerId;
				}
				samplerDescIt = samplerDescs.insert(samplerDescs.end(), desc);
			}
			return kModelSamplerSlots[static_cast<size_t>(samplerDescIt - samplerDescs.begin())];
		};

		// each distinct view once, from 1 (0 is material 0's, see InstallImage)
		std::vector<TextureView> views;
		std::vector<AnimatedTextureView> animatedViews;
		core::UnorderedMap<TextureViewKey, uint32_t, TextureViewKeyHash> viewIds;
		bool viewsFull = false;
		auto viewOf = [&](const Texture& texture, const TextureRef& ref) -> std::optional<uint32_t>
		{
			auto textureSlot = slotOf(texture);
			if (!textureSlot)
				return std::nullopt;

			auto samplerSlot = samplerSlotOf(ref.sampler);
			auto [viewIdIt, inserted] = viewIds.try_emplace(
				TextureViewKey{
					.textureSlot = *textureSlot,
					.samplerSlot = samplerSlot,
					.texCoord = ref.texCoord,
					.transform = std::bit_cast<std::array<uint32_t, 6>>(ref.transform),
					.animated = static_cast<uint32_t>(ref.animatedTransform + 1),
					.flags = texture.normalYUp ? TEXTURE_VIEW_FLAG_NORMAL_Y_UP : 0U},
				0U);
			if (inserted)
			{
				if (1 + views.size() == SHADER_TYPES_TEXTURE_VIEW_COUNT)
				{
					if (!std::exchange(viewsFull, true))
						std::println(stderr, "more than {} texture views, the rest are left out", SHADER_TYPES_TEXTURE_VIEW_COUNT - 1);
					viewIds.erase(viewIdIt);
					return std::nullopt;
				}
				viewIdIt->second = static_cast<uint32_t>(1 + views.size());
				views.push_back(MakeTextureView(*textureSlot, samplerSlot, ref, texture.normalYUp ? TEXTURE_VIEW_FLAG_NORMAL_Y_UP : 0U));
				if (ref.animatedTransform >= 0)
					animatedViews.push_back({.view = viewIdIt->second, .target = static_cast<uint32_t>(ref.animatedTransform), .data = views.back()});
			}
			return viewIdIt->second;
		};

		std::vector<MaterialData> materials(std::min<size_t>(textures.size(), kModelMaterialMaxCount), DefaultMaterialData());
		for (size_t materialIt = 0; materialIt < materials.size(); materialIt++)
		{
			auto& material = materials[materialIt];
			const auto& desc = descMaterials[materialIt];
			std::ranges::fill(material.color, 1.0F);
			material.alphaCutoff = desc.alphaCutoff;
			std::ranges::copy(desc.emissive, material.emissive);
			material.metallic = desc.metallic;
			material.roughness = desc.roughness;
			material.specular = desc.specular;
			std::ranges::copy(desc.specularColor, material.specularColor);
			material.specularColor[3] = desc.ior;
			if (desc.specularGlossiness)
			{
				material.flags |= MATERIAL_FLAG_SPECULAR_GLOSSINESS;
				material.roughness = desc.glossiness;
			}
			if (auto view = viewOf(textures[materialIt].specular, desc.specularTexture))
			{
				material.specularView = *view;
				material.flags |= MATERIAL_FLAG_SPECULAR_TEXTURE;
			}
			if (auto view = viewOf(textures[materialIt].specularColor, desc.specularColorTexture))
			{
				material.specularColorView = *view;
				material.flags |= MATERIAL_FLAG_SPECULAR_COLOR_TEXTURE;
			}
			auto layers = LayersOf(model->GetDesc(), materialIt);
			if (layers.clearcoat)
			{
				material.flags |= MATERIAL_FLAG_CLEARCOAT;
				material.clearcoat[0] = desc.clearcoat;
				material.clearcoat[1] = desc.clearcoatRoughness;
				material.clearcoat[2] = desc.clearcoatNormalScale;
				for (auto [texture, ref, view, flag] : std::array{
						 std::tuple{&textures[materialIt].clearcoat, &desc.clearcoatTexture, &material.clearcoatView, MATERIAL_FLAG_CLEARCOAT_TEXTURE},
						 std::tuple{&textures[materialIt].clearcoatRoughness, &desc.clearcoatRoughnessTexture, &material.clearcoatRoughnessView, MATERIAL_FLAG_CLEARCOAT_ROUGHNESS_TEXTURE},
						 std::tuple{&textures[materialIt].clearcoatNormal, &desc.clearcoatNormalTexture, &material.clearcoatNormalView, MATERIAL_FLAG_CLEARCOAT_NORMAL_TEXTURE}})
					if (auto id = viewOf(*texture, *ref))
					{
						*view = *id;
						material.flags |= flag;
					}
			}
			if (layers.sheen)
			{
				material.flags |= MATERIAL_FLAG_SHEEN;
				std::ranges::copy(desc.sheenColor, material.sheen);
				material.sheen[3] = desc.sheenRoughness;
				for (auto [texture, ref, view, flag] : std::array{
						 std::tuple{&textures[materialIt].sheenColor, &desc.sheenColorTexture, &material.sheenColorView, MATERIAL_FLAG_SHEEN_COLOR_TEXTURE},
						 std::tuple{&textures[materialIt].sheenRoughness, &desc.sheenRoughnessTexture, &material.sheenRoughnessView, MATERIAL_FLAG_SHEEN_ROUGHNESS_TEXTURE}})
					if (auto id = viewOf(*texture, *ref))
					{
						*view = *id;
						material.flags |= flag;
					}
			}
			if (layers.transmission)
			{
				material.flags |= MATERIAL_FLAG_TRANSMISSION;
				material.transmission[0] = desc.transmission;
				material.transmission[1] = desc.thickness;
				material.transmission[2] = desc.attenuationDistance;
				material.transmission[3] = desc.dispersion;
				std::ranges::copy(desc.attenuationColor, material.attenuationColor);
				for (auto [texture, ref, view, flag] : std::array{
						 std::tuple{&textures[materialIt].transmission, &desc.transmissionTexture, &material.transmissionView, MATERIAL_FLAG_TRANSMISSION_TEXTURE},
						 std::tuple{&textures[materialIt].thickness, &desc.thicknessTexture, &material.thicknessView, MATERIAL_FLAG_THICKNESS_TEXTURE}})
					if (auto id = viewOf(*texture, *ref))
					{
						*view = *id;
						material.flags |= flag;
					}
			}
			if (layers.anisotropy)
			{
				material.flags |= MATERIAL_FLAG_ANISOTROPY;
				material.anisotropy[0] = desc.anisotropy;
				material.anisotropy[1] = std::cos(desc.anisotropyRotation);
				material.anisotropy[2] = std::sin(desc.anisotropyRotation);
				if (auto id = viewOf(textures[materialIt].anisotropy, desc.anisotropyTexture))
				{
					material.anisotropyView = *id;
					material.flags |= MATERIAL_FLAG_ANISOTROPY_TEXTURE;
				}
			}
			if (layers.iridescence)
			{
				material.flags |= MATERIAL_FLAG_IRIDESCENCE;
				material.iridescence[0] = desc.iridescence;
				material.iridescence[1] = desc.iridescenceIor;
				material.iridescence[2] = desc.iridescenceThicknessMin;
				material.iridescence[3] = desc.iridescenceThicknessMax;
				for (auto [texture, ref, view, flag] : std::array{
						 std::tuple{&textures[materialIt].iridescence, &desc.iridescenceTexture, &material.iridescenceView, MATERIAL_FLAG_IRIDESCENCE_TEXTURE},
						 std::tuple{&textures[materialIt].iridescenceThickness, &desc.iridescenceThicknessTexture, &material.iridescenceThicknessView, MATERIAL_FLAG_IRIDESCENCE_THICKNESS_TEXTURE}})
					if (auto id = viewOf(*texture, *ref))
					{
						*view = *id;
						material.flags |= flag;
					}
			}
			if (layers.diffuseTransmission)
			{
				material.flags |= MATERIAL_FLAG_DIFFUSE_TRANSMISSION;
				std::ranges::copy(desc.diffuseTransmissionColor, material.diffuseTransmission);
				material.diffuseTransmission[3] = desc.diffuseTransmission;
				for (auto [texture, ref, view, flag] : std::array{
						 std::tuple{&textures[materialIt].diffuseTransmission, &desc.diffuseTransmissionTexture, &material.diffuseTransmissionView, MATERIAL_FLAG_DIFFUSE_TRANSMISSION_TEXTURE},
						 std::tuple{&textures[materialIt].diffuseTransmissionColor, &desc.diffuseTransmissionColorTexture, &material.diffuseTransmissionColorView, MATERIAL_FLAG_DIFFUSE_TRANSMISSION_COLOR_TEXTURE}})
					if (auto id = viewOf(*texture, *ref))
					{
						*view = *id;
						material.flags |= flag;
					}
			}
			if (desc.unlit)
				material.flags |= MATERIAL_FLAG_UNLIT;
			if (auto view = viewOf(textures[materialIt].metallicRoughness, desc.metallicRoughnessTexture))
			{
				material.metallicRoughnessView = *view;
				material.flags |= MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE;
			}

			if (auto view = viewOf(textures[materialIt].diffuse, desc.diffuseTexture))
			{
				material.baseColorView = *view;
				material.flags |= MATERIAL_FLAG_TEXTURE;
			}
			if (auto view = viewOf(textures[materialIt].alpha, desc.alphaTexture))
			{
				material.alphaView = *view;
				material.flags |= MATERIAL_FLAG_ALPHA_TEXTURE;
			}
			// the normal map, or a bump texture turned into one (see LoadAndInstallModels)
			if (auto view = viewOf(textures[materialIt].normal, desc.normalTexture.empty() ? desc.bumpTexture : desc.normalTexture))
			{
				material.normalView = *view;
				material.normalScale = desc.normalScale;
				material.flags |= MATERIAL_FLAG_NORMAL_TEXTURE;
			}
			if (auto view = viewOf(textures[materialIt].emissive, desc.emissiveTexture))
			{
				material.emissiveView = *view;
				material.flags |= MATERIAL_FLAG_EMISSIVE_TEXTURE;
			}
			if (auto view = viewOf(textures[materialIt].occlusion, desc.occlusionTexture))
			{
				material.occlusionView = *view;
				material.emissive[3] = desc.occlusionStrength;
				material.flags |= MATERIAL_FLAG_OCCLUSION_TEXTURE;
			}
		}

		// the model's samplers in their slots, and the previous model's other slots back to the default sampler
		auto samplers = std::make_shared<SamplerVector>(
			SamplerVectorCreateDesc{device.CreateDeviceObjectCreateDesc("Model Samplers"), std::vector(samplerDescs)});
		const auto& defaultSampler = (*device.GetResource<SamplerVector>(gSamplersUuid))[0];
		for (size_t slotIt = 0; slotIt < kModelSamplerSlots.size(); slotIt++)
			if (slotIt < samplerDescs.size() || slotIt < gModelSamplerCount)
				pipeline.SetDescriptorData(
					"gSamplers",
					ImageBinding{.sampler = slotIt < samplerDescs.size() ? (*samplers)[slotIt] : defaultSampler},
					DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
					kModelSamplerSlots[slotIt]);
		RetireAfterGraphicsWork(graphics, device.ReplaceResource(gModelSamplersUuid, samplers));
		gModelSamplersUuid = samplers->GetUuid();
		gModelSamplerCount = samplerDescs.size();

		UpdateTextureViews(rhi, graphics, 1, views);
		gAnimatedTextureViews = std::move(animatedViews);

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
		gModelMaterialData = materials;
		gAnimatedLights.clear();
		UpdateLights(rhi, graphics, model->GetDesc().lights);
		// and the values animations set, at rest
		ApplyPointerValues(rhi, graphics, *model);

		pipeline.SetDescriptorData(
			"gVertexBuffer",
			BufferBinding{.buffer = model->GetVertexBuffer(), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		pipeline.SetDescriptorData(
			"gSkinVertices",
			BufferBinding{
				.buffer = model->GetSkinBuffer() != nullptr ? *model->GetSkinBuffer() : *device.GetResource<Buffer>(gDefaultSkinVerticesUuid),
				.offset = 0,},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		pipeline.SetDescriptorData(
			"gMorphDeltas",
			BufferBinding{
				.buffer = model->GetMorphDeltaBuffer() != nullptr ? *model->GetMorphDeltaBuffer()
																  : *device.GetResource<Buffer>(gDefaultMorphDeltasUuid),
				.offset = 0,},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
		{
			pipeline.SetDescriptorData(
				"gModelInstances",
				BufferBinding{.buffer = model->GetInstanceBuffer(frameIt), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
			pipeline.SetDescriptorData(
				"gJointMatrices",
				BufferBinding{
					.buffer = model->GetJointBuffer(frameIt) != nullptr ? *model->GetJointBuffer(frameIt)
																		: *device.GetResource<Buffer>(gDefaultJointsUuid),
					.offset = 0,},
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
			pipeline.SetDescriptorData(
				"gMorphWeights",
				BufferBinding{
					.buffer = model->GetMorphWeightBuffer(frameIt) != nullptr ? *model->GetMorphWeightBuffer(frameIt)
																			  : *device.GetResource<Buffer>(gDefaultMorphWeightsUuid),
					.offset = 0,},
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
		}

		{
			auto scenes = gScenes.Write();
			scenes.Get() = SceneState{
				.filePath = model->GetDesc().name, .names = model->GetDesc().scenes, .current = model->GetDesc().scene};
		}

		// its first animation plays, from the start (SPEEDO_ANIMATION_TIME: paused at that time, e.g. for tests)
		{
			auto animation = gAnimation.Write();
			auto& state = animation.Get();
			state.names.clear();
			for (const auto& clip : model->GetDesc().animation.animations)
				state.names.push_back(clip.name);
			state.selected = state.names.empty() ? std::nullopt : std::optional<size_t>(0);
			state.playing = true;
			state.time = 0.0;
			state.last = std::chrono::steady_clock::now();
			state.previous.reset();
			state.fade = AnimationState::kCrossfade;
			if (const char* time = std::getenv("SPEEDO_ANIMATION_TIME"); time != nullptr && *time != '\0')
			{
				state.playing = false;
				state.time = std::strtod(time, nullptr);
			}
		}

		RetireAfterGraphicsWork(graphics, std::exchange(gModel, model));

		App().GetViews().SetScene(model->GetDesc().bounds, model->GetDesc().cameras);
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

		// the default material is untextured until an image is loaded. it samples it through view 0.
		auto view = MakeTextureView(kMaterialTextureId, kDefaultSamplerId, TextureRef{});
		UpdateTextureViews(rhi, graphics, 0, std::span(&view, 1));
		MaterialData material = DefaultMaterialData();
		material.flags = MATERIAL_FLAG_TEXTURE;
		material.alphaCutoff = 0.5F;
		material.baseColorView = 0;
		UpdateMaterials(rhi, graphics, 0, std::span(&material, 1));

		RetireAfterGraphicsWork(graphics, device.ReplaceResource(gLoadedImageUuid, image));
		RetireAfterGraphicsWork(graphics, device.ReplaceResource(gLoadedImageViewUuid, imageView));
		gLoadedImageUuid = image->GetUuid();
		gLoadedImageViewUuid = imageView->GetUuid();
	});
}

// loads a model (or several, side by side as one, see Model::Load) and its materials' textures, and has the draw thread
// install them, unless the load was cancelled. call from a load (see gLoads).
// scene: a gltf file's scene to load (one file only), else its default one
static void LoadAndInstallModels(
	RHI& rhi, const std::vector<std::string>& filePaths, std::atomic_uint8_t& progress, std::optional<size_t> scene = std::nullopt)
{
	auto model = scene && filePaths.size() == 1
					 ? Model::Load(filePaths.front(), progress, scene)
					 : Model::Load(std::vector<std::string_view>(filePaths.begin(), filePaths.end()), progress);
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
		std::optional<uint32_t> embeddedImage; // see TextureRef::embeddedImage
		gfx::image::Options options;
		Texture* result;
	};
	std::vector<MaterialTextures> textures(materials.size());
	std::vector<TextureLoad> loads;
	for (size_t materialIt = 0; materialIt < materials.size(); materialIt++)
	{
		const auto& material = materials[materialIt];
		auto& texture = textures[materialIt];
		auto layers = LayersOf(model->GetDesc(), materialIt);
		if (!material.diffuseTexture.empty())
			loads.push_back(
				{material.diffuseTexture.path, material.diffuseTexture.embeddedImage, {.usage = gfx::image::Usage::kColor}, &texture.diffuse});
		if (!material.alphaTexture.empty())
			loads.push_back(
				{material.alphaTexture.path, material.alphaTexture.embeddedImage, {.usage = gfx::image::Usage::kMask}, &texture.alpha});
		if (!material.normalTexture.empty())
			loads.push_back(
				{material.normalTexture.path, material.normalTexture.embeddedImage, {.usage = gfx::image::Usage::kNormal}, &texture.normal});
		else if (!material.bumpTexture.empty())
			loads.push_back(
				{material.bumpTexture.path, material.bumpTexture.embeddedImage, {.usage = gfx::image::Usage::kBump, .bumpScale = material.bumpScale}, &texture.normal});
		// the texture scales emissive, so it is only worth loading if that isn't black (obj map_Ke usually comes with Ke 0)
		if (!material.emissiveTexture.empty() && std::ranges::any_of(material.emissive, [](float value) { return value > 0.0F; }))
			loads.push_back(
				{material.emissiveTexture.path, material.emissiveTexture.embeddedImage, {.usage = gfx::image::Usage::kColor}, &texture.emissive});
		if (!material.occlusionTexture.empty())
			loads.push_back(
				{material.occlusionTexture.path, material.occlusionTexture.embeddedImage, {.usage = gfx::image::Usage::kOcclusion}, &texture.occlusion});
		if (!material.metallicRoughnessTexture.empty())
			loads.push_back(
				{material.metallicRoughnessTexture.path, material.metallicRoughnessTexture.embeddedImage, {.usage = gfx::image::Usage::kMetallicRoughness}, &texture.metallicRoughness});
		if (!material.specularTexture.empty())
			loads.push_back(
				{material.specularTexture.path, material.specularTexture.embeddedImage, {.usage = gfx::image::Usage::kAlpha}, &texture.specular});
		if (!material.specularColorTexture.empty())
			loads.push_back(
				{material.specularColorTexture.path, material.specularColorTexture.embeddedImage, {.usage = gfx::image::Usage::kColor}, &texture.specularColor});
		// the clearcoat's strength is the texture's red, its roughness the green (as kOcclusion and kMetallicRoughness
		// keep them), the sheen roughness the alpha
		if (layers.clearcoat)
			for (auto [ref, usage, result] : std::array{
					 std::tuple{&material.clearcoatTexture, gfx::image::Usage::kOcclusion, &texture.clearcoat},
					 std::tuple{&material.clearcoatRoughnessTexture, gfx::image::Usage::kMetallicRoughness, &texture.clearcoatRoughness},
					 std::tuple{&material.clearcoatNormalTexture, gfx::image::Usage::kNormal, &texture.clearcoatNormal}})
				if (!ref->empty())
					loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
		// the anisotropy's direction and strength are a linear rgb texture; the iridescence's factor is the red, its
		// thickness the green
		if (layers.anisotropy && !material.anisotropyTexture.empty())
			loads.push_back(
				{material.anisotropyTexture.path, material.anisotropyTexture.embeddedImage, {.usage = gfx::image::Usage::kLinear}, &texture.anisotropy});
		if (layers.iridescence)
			for (auto [ref, usage, result] : std::array{
					 std::tuple{&material.iridescenceTexture, gfx::image::Usage::kOcclusion, &texture.iridescence},
					 std::tuple{&material.iridescenceThicknessTexture, gfx::image::Usage::kMetallicRoughness, &texture.iridescenceThickness}})
				if (!ref->empty())
					loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
		// the diffuse transmission's factor is the texture's alpha, its color srgb
		if (layers.diffuseTransmission)
			for (auto [ref, usage, result] : std::array{
					 std::tuple{&material.diffuseTransmissionTexture, gfx::image::Usage::kAlpha, &texture.diffuseTransmission},
					 std::tuple{&material.diffuseTransmissionColorTexture, gfx::image::Usage::kColor, &texture.diffuseTransmissionColor}})
				if (!ref->empty())
					loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
		// the transmission's factor is the texture's red, the thickness the green
		if (layers.transmission)
			for (auto [ref, usage, result] : std::array{
					 std::tuple{&material.transmissionTexture, gfx::image::Usage::kOcclusion, &texture.transmission},
					 std::tuple{&material.thicknessTexture, gfx::image::Usage::kMetallicRoughness, &texture.thickness}})
				if (!ref->empty())
					loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
		if (layers.sheen)
			for (auto [ref, usage, result] : std::array{
					 std::tuple{&material.sheenColorTexture, gfx::image::Usage::kColor, &texture.sheenColor},
					 std::tuple{&material.sheenRoughnessTexture, gfx::image::Usage::kAlpha, &texture.sheenRoughness}})
				if (!ref->empty())
					loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
	}

	core::UnorderedMap<std::string, Texture> loaded;
	progress = 0;
	for (size_t loadIt = 0; loadIt < loads.size(); loadIt++)
	{
		const auto& load = loads[loadIt];
		auto key = std::format(
			"{}|{}|{}|{}", load.path, load.embeddedImage.value_or(~0U), std::to_underlying(load.options.usage), load.options.bumpScale);
		auto [it, inserted] = loaded.try_emplace(std::move(key));
		if (inserted)
		{
			if (core::Application::Get()->IsExitRequested())
				return;

			std::atomic_uint8_t textureProgress = 0;
			it->second = LoadTexture(load.path, textureProgress, load.options, load.embeddedImage);
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

// loads the models below a directory (e.g. the encodings of a gltf sample model) as one, side by side (see Model::Load).
// call from a load (see gLoads).
static void LoadAndInstallFolder(RHI& rhi, std::string_view directoryPath, std::atomic_uint8_t& progress)
{
	auto models = FindModels(directoryPath);
	if (models.empty())
	{
		std::println(stderr, "Failed to load folder {}: it holds no model files", directoryPath);
		return;
	}

	std::vector<std::string> paths;
	for (const auto& model : models)
		paths.push_back(model.string());
	LoadAndInstallModels(rhi, paths, progress);
}

// loads an image and has the draw thread install it, unless the load was cancelled. call from a load (see gLoads).
static void LoadAndInstallImage(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress)
{
	auto [image, imageView, upload, normalYUp] = LoadTexture(filePath, progress);
	if (!image) // cancelled or failed
		return;

	auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData*>(
		[&rhi, image, imageView, upload](QueueTimelineContextData* graphics)
		{ InstallImage(rhi, *graphics, image, imageView, upload); });
	rhi.drawCalls.enqueue(installTask);
}

// makes an uploaded environment the one the scene is lit by, retiring the previous one. call on the draw thread.
static void InstallEnvironment(
	RHI& rhi, QueueTimelineContextData& graphics, const std::shared_ptr<const EnvironmentTexture>& environment, std::string name)
{
	ZoneScopedN("WindowedApplication::InstallEnvironment");

	TransitionThenBind(
		rhi,
		graphics,
		Uploads{
			.images =
				{{environment->texture.image, environment->texture.upload},
				 {environment->sheenTexture.image, environment->sheenTexture.upload}}},
		[&rhi, environment, name = std::move(name)](QueueTimelineContextData& graphics) mutable
		{
			auto& device = rhi.GetPrimaryDevice();
			auto& pipeline = device.GetPipeline();
			const auto& [image, view, upload, normalYUp] = environment->texture;
			const auto& [sheenImage, sheenView, sheenUpload, sheenNormalYUp] = environment->sheenTexture;

			pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);
			pipeline.SetDescriptorData(
				"gTextures",
				ImageBinding{.sampler = {}, .imageView = *view, .layout = image->GetDesc().layout},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				SHADER_TYPES_ENVIRONMENT_TEXTURE);
			pipeline.SetDescriptorData(
				"gTextures",
				ImageBinding{.sampler = {}, .imageView = *sheenView, .layout = sheenImage->GetDesc().layout},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE);

			EnvironmentData data{
				.textureId = SHADER_TYPES_ENVIRONMENT_TEXTURE,
				.samplerId = kDefaultSamplerId,
				.levelCount = static_cast<float>(environment->levelCount),
				.sheenLevelCount = static_cast<float>(environment->sheenLevelCount)};
			for (size_t i = 0; i < environment->irradiance.size(); i++)
				std::ranges::copy(environment->irradiance[i], data.irradiance[i]);
			UpdateBufferOnGraphics(
				graphics, *device.GetResource<Buffer>(gEnvironmentUuid), 0, std::as_bytes(std::span(&data, 1)));

			RetireAfterGraphicsWork(graphics, device.ReplaceResource(gEnvironmentImageUuid, image));
			RetireAfterGraphicsWork(graphics, device.ReplaceResource(gEnvironmentViewUuid, view));
			RetireAfterGraphicsWork(graphics, device.ReplaceResource(gEnvironmentSheenImageUuid, sheenImage));
			RetireAfterGraphicsWork(graphics, device.ReplaceResource(gEnvironmentSheenViewUuid, sheenView));
			gEnvironmentImageUuid = image->GetUuid();
			gEnvironmentViewUuid = view->GetUuid();
			gEnvironmentSheenImageUuid = sheenImage->GetUuid();
			gEnvironmentSheenViewUuid = sheenView->GetUuid();
			gEnvironmentName.Write().Get() = std::move(name);
		});
}

// loads an environment panorama (or without one, the procedural sky) and has the draw thread install it, unless the
// load was cancelled. call from a load (see gLoads).
static void LoadAndInstallEnvironment(RHI& rhi, std::optional<std::string> filePath, std::atomic_uint8_t& progress)
{
	auto environment = std::make_shared<const EnvironmentTexture>(
		LoadEnvironment(filePath ? std::optional<std::string_view>(*filePath) : std::nullopt, progress));
	if (!*environment) // cancelled or failed
		return;

	auto name = filePath ? std::filesystem::path(*filePath).filename().string() : std::string("Procedural sky");
	auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData*>(
		[&rhi, environment = std::move(environment), name = std::move(name)](QueueTimelineContextData* graphics)
		{ InstallEnvironment(rhi, *graphics, environment, name); });
	rhi.drawCalls.enqueue(installTask);
}

// the environment panorama files LoadAndInstallEnvironment takes, as a file dialog filter spec
static constexpr const char* kEnvironmentExtensions = "hdr";

// the image files LoadAndInstallImage takes, as a file dialog filter spec (see image::Import)
static constexpr const char* kImageExtensions = "jpg,jpeg,png,bmp,tga,gif,psd,pic,pnm,webp,ktx2";

[[nodiscard]] static bool IsImageFile(const std::filesystem::path& path)
{
	auto extension = path.extension().string();
	if (extension.empty())
		return false;
	std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::string_view extensions = kImageExtensions;
	for (size_t begin = 0; begin < extensions.size();)
	{
		auto end = std::min(extensions.find(',', begin), extensions.size());
		if (extensions.substr(begin, end - begin) == std::string_view(extension).substr(1))
			return true;
		begin = end + 1;
	}
	return false;
}

// loads whatever path is, by its type: a directory's models or a zip archive's (see ArchiveModels), a model, an
// environment panorama (.hdr), or an image (on the default material). call from a load (see gLoads).
static void LoadAndInstallFile(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress, ArchiveModels several)
{
	std::filesystem::path path(filePath);
	auto extension = path.extension().string();
	std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (std::error_code error; std::filesystem::is_directory(path, error))
		LoadAndInstallFolder(rhi, filePath, progress);
	else if (extension == ".zip")
		LoadAndInstallArchive(rhi, filePath, progress, several);
	else if (mesh::IsModelFile(path))
		LoadAndInstallModel(rhi, filePath, progress);
	else if (extension == ".hdr")
		LoadAndInstallEnvironment(rhi, std::string(filePath), progress);
	else if (IsImageFile(path))
		LoadAndInstallImage(rhi, filePath, progress);
	else
		std::println(stderr, "Failed to load file {}: not a model, zip archive, environment or image", filePath);
}

static void DrawMainPass(
	RHI& rhi,
	Window& window,
	Pipeline& pipeline,
	Queue& graphicsQueue,
	CommandBufferHandle cmd,
	uint16_t newFrameIndex,
	uint64_t graphicsTimeline,
	MainPassPhase phase)
{
	GPU_SCOPE(cmd, graphicsQueue, draw);

	auto& device = rhi.GetPrimaryDevice();
	auto& renderImageSet = *device.GetResource<RenderImageSet>(gRenderImageSetUuids[newFrameIndex]);

	// the second phase continues what the first drew
	auto loadOp = phase == MainPassPhase::kOpaque ? LoadOp::kClear : LoadOp::kLoad;
	renderImageSet.SetLoadOp(loadOp, kColorAttachment);
	renderImageSet.SetStoreOp(StoreOp::kStore, kColorAttachment);
	renderImageSet.Transition(cmd, ImageLayout::kColorAttachment, ImageAspect::kColor, kColorAttachment);
	renderImageSet.SetLoadOp(loadOp, kDepthAttachment, loadOp);
	renderImageSet.SetStoreOp(StoreOp::kStore, kDepthAttachment, StoreOp::kStore);
	renderImageSet.Transition(cmd, ImageLayout::kDepthStencilAttachment, ImageAspect::kDepth | ImageAspect::kStencil, kDepthAttachment);

	pipeline.SetRenderTarget(renderImageSet);

	auto renderTargetInfo = renderImageSet.Begin(cmd, SubpassContents::kSecondaryCommandBuffers);

	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);

	// setup draw parameters
	const auto grid = App().GetViews().GetGrid();
	const auto framePushConstants = FramePushConstants(newFrameIndex);
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

		// the views are drawn in their viewports: their grid cells, letterboxed to their cameras' aspect ratios
		auto viewports = App().GetViews().GetViewports();

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
			&viewports,
			&framePushConstants,
			phase,
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
					pipeline.BindPipelineAuto(cmd, {.blend = kOpaqueBlend});
				};

				bindState(cmd);

				PushConstants pushConstants = framePushConstants;

				ASSERT(deltaX > 0);
				ASSERT(deltaY > 0);

				while (drawIt < drawCount)
				{
					auto drawView = [&pushConstants, &pipeline, &model, &cmd, &encoder, &deltaX, &deltaY, &viewports, phase, grid](uint16_t viewIt)
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
							auto width = deltaX;
							auto height = deltaY;
							if (viewIt < viewports.size() && viewports[viewIt].width > 0 && viewports[viewIt].height > 0)
							{
								posX = viewports[viewIt].x;
								posY = viewports[viewIt].y;
								width = viewports[viewIt].width;
								height = viewports[viewIt].height;
							}
							encoder.SetViewport(Viewport{
								.x = static_cast<float>(posX),
								.y = static_cast<float>(posY),
								.width = static_cast<float>(width),
								.height = static_cast<float>(height),
								.minDepth = 0.0F,
								.maxDepth = 1.0F});
							encoder.SetScissor(rhi::Rect{.x = posX, .y = posY, .width = width, .height = height});
						}

						uint16_t viewIndex = viewIt;

						// one draw per submesh (material and topology, see InstallModel for where the materials are): the opaque
						// ones first, then the blended ones back to front by their centers (each as a whole: the triangles within
						// one are drawn in their order)
						auto drawModel = [&pushConstants, &pipeline, &model, &encoder, viewIndex, phase](CommandBufferHandle cmd)
						{
							ZoneScopedN("drawModel");

							const auto& materials = model.GetDesc().materials;
							const auto& skins = model.GetDesc().animation.skins;
							// bindState bound the default (opaque triangle list) pipeline
							GraphicsPipelineVariant bound{.blend = kOpaqueBlend};
							auto draw = [&](const ModelSubmesh& submesh, const std::array<BlendMode, kMaxColorAttachments>& blend, uint8_t fragmentShader = 0)
							{
								if (GraphicsPipelineVariant variant{.topology = submesh.topology, .blend = blend, .fragmentShader = fragmentShader}; variant != bound)
								{
									bound = variant;
									pipeline.BindPipelineAuto(cmd, variant);
								}

								// double sided materials' back faces are drawn too (the cull mode is dynamic state)
								bool doubleSided = submesh.material >= 0 && materials[submesh.material].doubleSided;
								encoder.SetCullMode(doubleSided ? CullMode::kNone : CullMode::kBack);

								pushConstants.viewAndMaterialId =
									(static_cast<uint32_t>(viewIndex) << SHADER_TYPES_MATERIAL_INDEX_BITS) | ModelMaterialSlot(submesh.material);

								// its instances (gModelInstances from modelInstanceId, by SV_InstanceID), the mirroring ones last and
								// separately: they reverse the winding, so their front faces are clockwise (dynamic state too)
								auto drawInstances = [&](uint32_t firstInstance, uint32_t instanceCount, FrontFace frontFace)
								{
									if (instanceCount == 0)
										return;
									encoder.SetFrontFace(frontFace);
									pushConstants.modelInstanceId = firstInstance;
									pushConstants.jointBase = submesh.skin >= 0 ? skins[submesh.skin].jointBase : SHADER_TYPES_NOT_SKINNED;
									pushConstants.morphTargetCount = submesh.morphTargetCount;
									pushConstants.morphDeltaBase = submesh.morphDeltaBase;
									pushConstants.morphFirstVertex = submesh.morphFirstVertex;
									pushConstants.morphWeightBase = submesh.morphWeightBase;
									pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));
									encoder.DrawIndexed(submesh.indexCount, instanceCount, submesh.firstIndex);
								};
								auto unmirrored = submesh.instanceCount - submesh.mirroredInstanceCount;
								drawInstances(submesh.firstInstance, unmirrored, FrontFace::kCounterClockwise);
								drawInstances(submesh.firstInstance + unmirrored, submesh.mirroredInstanceCount, FrontFace::kClockwise);
							};

							// the opaque submeshes, then the blended ones (in any order, see kTransparentBlend), which depth test
							// against them. with transmissive ones (see MainPassPhase), those and the blended ones in the second
							// phase
							bool split = HasTransmission(model);
							auto blended = [&materials](const ModelSubmesh& submesh)
							{ return submesh.material >= 0 && materials[submesh.material].blend; };
							auto transmissive = [&materials](const ModelSubmesh& submesh)
							{ return submesh.material >= 0 && materials[submesh.material].transmission > 0.0F; };
							for (const auto& submesh : model.GetDesc().submeshes)
								if (!blended(submesh) &&
									(split && transmissive(submesh) ? MainPassPhase::kTransmissive : MainPassPhase::kOpaque) == phase)
									draw(submesh, kOpaqueBlend);
							if (!split || phase == MainPassPhase::kTransmissive)
								for (const auto& submesh : model.GetDesc().submeshes)
									if (blended(submesh))
										draw(submesh, kTransparentBlend, kTransparentFragmentShader);

							if (bound != GraphicsPipelineVariant{.blend = kOpaqueBlend})
								pipeline.BindPipelineAuto(cmd, {.blend = kOpaqueBlend});
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
		auto renderImageSet = device.CreateResource<RenderImageSet>(
			std::move(colorImage), std::move(depthStencilImage));
		ENSURE(renderImageSet->GetAttachments().size() == kDepthAttachment + 1);
		// alpha 0: where nothing opaque is drawn (opaque draws write alpha 1), ComputeMain draws the environment
		renderImageSet->SetClearValue(ClearValue{.color = {0.0F, 0.0F, 0.0F, 0.0F}}, kColorAttachment);
		gRenderImageSetUuids[frameIt] = renderImageSet->GetUuid();
	}

	// the order independent transparency's lists, for the render target's pixels (replacing any previous ones)
	{
		auto extent = window.GetSwapchain().GetDesc().extent;
		auto pixelCount = static_cast<uint64_t>(extent.width) * extent.height;
		gOitWidth = extent.width;
		gOitNodeCapacity = static_cast<uint32_t>(std::min<uint64_t>(pixelCount * SHADER_TYPES_OIT_NODES_PER_PIXEL, SHADER_TYPES_OIT_NONE - 1));
		auto create = [&device](uuids::uuid& uuid, std::string_view name, uint64_t size)
		{
			device.EraseResource(uuid);
			uuid = device.CreateResource<Buffer>(BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc(name),
				size,
				BufferUsage::kStorage | BufferUsage::kTransferDestination,
				MemoryProperty::kDeviceLocal})->GetUuid();
		};
		create(gOitHeadsUuid, "OIT Heads", std::max<uint64_t>(pixelCount, 1) * sizeof(uint32_t));
		create(gOitNodesUuid, "OIT Nodes", std::max<uint64_t>(gOitNodeCapacity, 1) * sizeof(OitNode));
		create(gOitCounterUuid, "OIT Counter", sizeof(uint32_t));

		// the transmission texture, a full mip chain
		std::vector<ImageMipLevelDesc> mipLevels;
		for (auto mip = extent; ; mip = {.width = std::max(mip.width / 2, 1U), .height = std::max(mip.height / 2, 1U)})
		{
			mipLevels.push_back({.extent = mip});
			if (mip.width == 1 && mip.height == 1)
				break;
		}
		device.EraseResource(gTransmissionViewUuid);
		device.EraseResource(gTransmissionImageUuid);
		auto transmissionImage = device.CreateResource<Image>(ImageCreateDesc{
			device.CreateDeviceObjectCreateDesc("Transmission"),
			std::move(mipLevels),
			Format::kR16G16B16A16Sfloat,
			ImageTiling::kOptimal,
			ImageUsage::kTransferSource | ImageUsage::kTransferDestination | ImageUsage::kSampled,
			MemoryProperty::kDeviceLocal,
			ImageAspect::kColor,
			ImageLayout::kUndefined});
		gTransmissionImageUuid = transmissionImage->GetUuid();
		gTransmissionViewUuid = device.CreateResource<ImageView>(ImageViewCreateDesc{
			device.CreateDeviceObjectCreateDesc("Transmission View"), *transmissionImage, Format::kR16G16B16A16Sfloat, ImageAspect::kColor})
									->GetUuid();
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
			renderImageSet.SetLoadOp(LoadOp::kClear, kColorAttachment);
			renderImageSet.SetStoreOp(StoreOp::kStore, kColorAttachment);
			renderImageSet.Transition(cmd, ImageLayout::kGeneral, ImageAspect::kColor, kColorAttachment);
			renderImageSet.SetLoadOp(LoadOp::kClear, kDepthAttachment, LoadOp::kClear);
			renderImageSet.SetStoreOp(StoreOp::kStore, kDepthAttachment, StoreOp::kStore);
			renderImageSet.Transition(cmd, ImageLayout::kGeneral, ImageAspect::kDepth | ImageAspect::kStencil, kDepthAttachment);
		}
		device.GetResource<Image>(gTransmissionImageUuid)->Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);

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
	pipeline.SetDescriptorData(
		"gTextures",
		ImageBinding{.sampler = {}, .imageView = *device.GetResource<ImageView>(gTransmissionViewUuid), .layout = ImageLayout::kShaderReadOnly},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
		SHADER_TYPES_TRANSMISSION_TEXTURE);
	for (auto [name, uuid] : std::array{
			 std::pair{"gOitHeads", gOitHeadsUuid}, std::pair{"gOitNodes", gOitNodesUuid}, std::pair{"gOitCounter", gOitCounterUuid},
			 std::pair{"gExposureHistogram", gExposureHistogramUuid}})
		pipeline.SetDescriptorData(
			name, BufferBinding{.buffer = *device.GetResource<Buffer>(uuid), .offset = 0}, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
	for (unsigned frameIt = 0; frameIt < frameCount; frameIt++)
	{
		auto& renderImageSet = *device.GetResource<RenderImageSet>(gRenderImageSetUuids[frameIt]);
		auto& frame = window.GetSwapchain().GetFrames()[frameIt];

		pipeline.SetDescriptorData(
			"gTextures",
			ImageBinding{
				.sampler={},
				.imageView=renderImageSet.GetAttachments()[kColorAttachment],
				.layout=ImageLayout::kShaderReadOnly},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			SHADER_TYPES_RENDER_TARGET_TEXTURE_BASE + frameIt);

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

	// the move speed for a moment after it changes (with the mouse wheel, or a newly framed model), at the bottom
	{
		using namespace std::chrono_literals;
		static float gShownMoveSpeed = myViews->GetMoveSpeed();
		static auto gMoveSpeedChanged = std::chrono::steady_clock::time_point{};
		auto now = std::chrono::steady_clock::now();
		if (auto moveSpeed = myViews->GetMoveSpeed(); moveSpeed != gShownMoveSpeed)
		{
			gShownMoveSpeed = moveSpeed;
			gMoveSpeedChanged = now;
		}
		if (now - gMoveSpeedChanged < 1500ms)
		{
			constexpr float kOverlayPadding = 10.0F;
			constexpr float kOverlayBgAlpha = 0.35F;
			const auto* viewport = GetMainViewport();
			SetNextWindowPos(
				ImVec2(viewport->WorkPos.x + (0.5F * viewport->WorkSize.x), viewport->WorkPos.y + viewport->WorkSize.y - kOverlayPadding),
				ImGuiCond_Always,
				ImVec2(0.5F, 1.0F));
			SetNextWindowBgAlpha(kOverlayBgAlpha);
			if (Begin(
					"Move speed",
					nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
						ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
				Text("Move speed: %.3g units/s", gShownMoveSpeed);
			End();
		}
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

	// the file dialogs open in the test asset sets, if they have been fetched (see scripts/fetch-test-assets.ps1)
	auto dialogPath = [&resourcePath]
	{
		std::error_code error;
		auto testAssets = resourcePath / "test-assets";
		return (std::filesystem::is_directory(testAssets, error) ? testAssets : resourcePath).string();
	};

	// automation: SPEEDO_AUTOLOAD_MODEL (a model, or a zip archive or directory, whose models are loaded side by side)
	// and SPEEDO_AUTOLOAD_IMAGE (an image, on the default material) name files to load at startup, absolute or relative to
	// the resource directory, through the same load + install path as the "File" menu (SPEEDO_AUTOLOAD_SCENE=<index>: a
	// gltf model's scene, as View > Scene loads it). with
	// SPEEDO_AUTOLOAD_EXIT=<frames>, the application exits that many frames after the loads have finished (see
	// scripts/assettest.ps1).
	static std::vector<core::Future<void>> gAutoLoads;
	static std::optional<uint32_t> gAutoLoadExitFrames;
	if (static bool gAutoLoadDone = false; !gAutoLoadDone)
	{
		gAutoLoadDone = true;

		// queued as separate loads, which run concurrently
		// a zip archive loads all of its models, side by side
		std::optional<size_t> autoLoadScene;
		if (const char* scene = std::getenv("SPEEDO_AUTOLOAD_SCENE"); scene != nullptr && *scene != '\0')
			autoLoadScene = std::strtoull(scene, nullptr, 10);
		if (const char* autoLoadModel = std::getenv("SPEEDO_AUTOLOAD_MODEL"); autoLoadModel != nullptr && *autoLoadModel != '\0')
			gAutoLoads.emplace_back(gLoads.Enqueue(
				autoLoadModel,
				[&rhi, path = (resourcePath / autoLoadModel).string(), autoLoadScene](std::atomic_uint8_t& progress)
				{
					if (autoLoadScene && mesh::IsModelFile(path))
						LoadAndInstallModels(rhi, {path}, progress, autoLoadScene);
					else
						LoadAndInstallFile(rhi, path, progress, ArchiveModels::kAll);
				}));
		// the tonemapper (SPEEDO_TONEMAPPER: pbr-neutral, aces, agx, reinhard or linear) and auto exposure
		// (SPEEDO_AUTO_EXPOSURE=1) to start with (as View sets them)
		if (const char* tonemapper = std::getenv("SPEEDO_TONEMAPPER"); tonemapper != nullptr)
		{
			std::string_view name(tonemapper);
			gTonemapper.store(
				name == "aces"		 ? TONEMAPPER_ACES
				: name == "agx"		 ? TONEMAPPER_AGX
				: name == "reinhard" ? TONEMAPPER_REINHARD
				: name == "linear"	 ? TONEMAPPER_LINEAR
									 : TONEMAPPER_PBR_NEUTRAL,
				std::memory_order_relaxed);
		}
		if (const char* autoExposure = std::getenv("SPEEDO_AUTO_EXPOSURE"); autoExposure != nullptr && std::string_view(autoExposure) == "1")
			gAutoExposure.store(true, std::memory_order_relaxed);
		// the environment's rotation (degrees) and intensity to start with (as View > Environment sets them)
		if (const char* rotation = std::getenv("SPEEDO_ENVIRONMENT_ROTATION"); rotation != nullptr && *rotation != '\0')
			gEnvironmentRotationDegrees.store(std::strtof(rotation, nullptr), std::memory_order_relaxed);
		if (const char* intensity = std::getenv("SPEEDO_ENVIRONMENT_INTENSITY"); intensity != nullptr && *intensity != '\0')
			gEnvironmentIntensity.store(std::max(std::strtof(intensity, nullptr), 0.0F), std::memory_order_relaxed);
		// the environment: SPEEDO_AUTOLOAD_ENVIRONMENT's panorama, else the procedural sky
		std::optional<std::string> environmentPath;
		if (const char* autoLoadEnvironment = std::getenv("SPEEDO_AUTOLOAD_ENVIRONMENT");
			autoLoadEnvironment != nullptr && *autoLoadEnvironment != '\0')
			environmentPath = (resourcePath / autoLoadEnvironment).string();
		gAutoLoads.emplace_back(gLoads.Enqueue(
			environmentPath.value_or("Procedural sky"),
			[&rhi, environmentPath](std::atomic_uint8_t& progress) { LoadAndInstallEnvironment(rhi, environmentPath, progress); }));
		if (const char* autoLoadImage = std::getenv("SPEEDO_AUTOLOAD_IMAGE"); autoLoadImage != nullptr && *autoLoadImage != '\0')
			gAutoLoads.emplace_back(gLoads.Enqueue(
				autoLoadImage,
				[&rhi, path = (resourcePath / autoLoadImage).string()](std::atomic_uint8_t& progress)
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
			if (MenuItem("Open File..."))
			{
				// models, zip archives (of models), environments and images: what is loaded depends on the file's type
				static const std::string kAllExtensions = std::format("obj,gltf,glb,zip,{},{}", kEnvironmentExtensions, kImageExtensions);
				static const std::vector<FileFilter> kFilterList = {
					FileFilter{.name = "Models, zip archives, environments and images", .spec = kAllExtensions.c_str()},
					FileFilter{.name = "Models (Wavefront OBJ, glTF)", .spec = "obj,gltf,glb"},
					FileFilter{.name = "Zip archives", .spec = "zip"},
					FileFilter{.name = "Environments (Radiance HDR panoramas)", .spec = kEnvironmentExtensions},
					FileFilter{.name = "Images", .spec = kImageExtensions},
				};
				InternalOpenFileDialogueAsync(dialogPath(), kFilterList,
					[&rhi](std::string_view filePath, std::atomic_uint8_t& progressOut)
					{ LoadAndInstallFile(rhi, filePath, progressOut, ArchiveModels::kChoose); });
			}
			if (MenuItem("Open Folder..."))
			{
				InternalOpenFolderDialogueAsync(dialogPath(),
					[&rhi](std::string_view directoryPath, std::atomic_uint8_t& progressOut)
					{ LoadAndInstallFolder(rhi, directoryPath, progressOut); });
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
			if (BeginMenu("Environment"))
			{
				// what lights the scene (see InstallEnvironment): the procedural sky, or a panorama file
				std::string current = gEnvironmentName.Read().Get();
				TextDisabled("%s", current.empty() ? "None" : current.c_str());
				{
					float intensity = gEnvironmentIntensity.load(std::memory_order_relaxed);
					if (DragFloat("Intensity", &intensity, 0.01F, 0.0F, 100.0F, "%.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
						gEnvironmentIntensity.store(intensity, std::memory_order_relaxed);
					SetItemTooltip("Scales its light. Drag, or double-click to type.");
					float rotation = gEnvironmentRotationDegrees.load(std::memory_order_relaxed);
					if (DragFloat("Rotation", &rotation, 0.5F, -180.0F, 180.0F, "%.1f deg", ImGuiSliderFlags_WrapAround))
						gEnvironmentRotationDegrees.store(rotation, std::memory_order_relaxed);
					SetItemTooltip("Turns it about the vertical axis. Drag, or double-click to type.");
					bool backdrop = gEnvironmentBackdrop.load(std::memory_order_relaxed);
					if (MenuItem("Backdrop", nullptr, &backdrop))
						gEnvironmentBackdrop.store(backdrop, std::memory_order_relaxed);
				}
				Separator();
				if (MenuItem("Procedural sky"))
					(void)gLoads.Enqueue(
						"Procedural sky",
						[&rhi](std::atomic_uint8_t& progress) { LoadAndInstallEnvironment(rhi, std::nullopt, progress); });
				if (MenuItem("Open Environment..."))
				{
					static const std::vector<FileFilter> kFilterList = {
						FileFilter{.name = "Environments (Radiance HDR panoramas)", .spec = kEnvironmentExtensions}};
					InternalOpenFileDialogueAsync(dialogPath(), kFilterList,
						[&rhi](std::string_view filePath, std::atomic_uint8_t& progressOut)
						{ LoadAndInstallEnvironment(rhi, std::string(filePath), progressOut); });
				}
				ImGui::EndMenu();
			}
			if (BeginMenu("Scene"))
			{
				// the installed gltf file's scenes: choosing one loads the file again with it
				SceneState scenes = gScenes.Read().Get();
				if (scenes.names.size() < 2)
					TextDisabled(scenes.names.empty() ? "No scenes" : "One scene");
				for (size_t sceneIt = 0; sceneIt < scenes.names.size(); sceneIt++)
				{
					PushID(static_cast<int>(sceneIt));
					if (MenuItem(scenes.names[sceneIt].c_str(), nullptr, scenes.current == sceneIt) && scenes.current != sceneIt)
						(void)gLoads.Enqueue(
							std::format("{} ({})", std::filesystem::path(scenes.filePath).filename().string(), scenes.names[sceneIt]),
							[&rhi, path = scenes.filePath, sceneIt](std::atomic_uint8_t& progress)
							{ LoadAndInstallModels(rhi, {path}, progress, sceneIt); });
					PopID();
				}
				ImGui::EndMenu();
			}
			if (BeginMenu("Animation"))
			{
				// the installed model's animations (see Model::Animate)
				auto animation = gAnimation.Write();
				auto& state = animation.Get();
				MenuItem("Play", nullptr, &state.playing);
				if (MenuItem("Restart"))
					state.time = 0.0;
				Separator();
				if (MenuItem("Rest pose", nullptr, !state.selected.has_value()))
					state.Select(std::nullopt);
				for (size_t animationIt = 0; animationIt < state.names.size(); animationIt++)
				{
					PushID(static_cast<int>(animationIt));
					if (MenuItem(state.names[animationIt].c_str(), nullptr, state.selected == animationIt))
						state.Select(animationIt);
					PopID();
				}
				ImGui::EndMenu();
			}
			if (BeginMenu("Camera"))
			{
				// the installed model's cameras (see Views::SetScene), applied on the draw thread
				auto useCamera = [this, &rhi](std::optional<size_t> camera)
				{
					auto [task, future] = core::CreateTask([this, camera] { myViews->UseSceneCamera(camera); });
					rhi.drawCalls.enqueue(task);
				};
				auto current = myViews->GetSceneCamera();
				if (MenuItem("Frame model", nullptr, !current.has_value()))
					useCamera(std::nullopt);
				auto names = myViews->GetSceneCameraNames();
				if (!names.empty())
					Separator();
				for (size_t cameraIt = 0; cameraIt < names.size(); cameraIt++)
				{
					PushID(static_cast<int>(cameraIt));
					if (MenuItem(names[cameraIt].c_str(), nullptr, current == cameraIt))
						useCamera(cameraIt);
					PopID();
				}
				ImGui::EndMenu();
			}
			MenuItem("FPS", nullptr, &gShowFps);
			MenuItem("TPS", nullptr, &gShowTps);
			Separator();
			{
				// logarithmic: model scales span many orders of magnitude (see Views::FrameBounds). a drag box rather than a
				// slider: a double-click (or Ctrl/Cmd+click) types a number into it, which a slider only does on
				// Ctrl/Cmd+click. its speed is in the logarithmic range's terms: about 1% faster or slower per pixel.
				constexpr float kMinMoveSpeed = 1e-4F;
				constexpr float kMaxMoveSpeed = 1e6F;
				constexpr float kDragPixelsAcrossRange = 1000.0F;
				float moveSpeed = myViews->GetMoveSpeed();
				if (DragFloat(
						"Move speed",
						&moveSpeed,
						(kMaxMoveSpeed - kMinMoveSpeed) / kDragPixelsAcrossRange,
						kMinMoveSpeed,
						kMaxMoveSpeed,
						"%.3g units/s",
						ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
					myViews->SetMoveSpeed(moveSpeed);
				SetItemTooltip(
					"Drag to change, or double-click to type a speed.\n"
					"w, a, s, d move the camera under the mouse, and the mouse wheel changes the speed.");
			}
			{
				float stops = gExposureStops.load(std::memory_order_relaxed);
				if (DragFloat("Exposure", &stops, 0.05F, -16.0F, 16.0F, "%+.2f EV", ImGuiSliderFlags_AlwaysClamp))
					gExposureStops.store(stops, std::memory_order_relaxed);
				SetItemTooltip("Scales the image by 2^EV before tonemapping (with auto exposure, on top of what it picks). Drag, or double-click to type.");
				bool autoExposure = gAutoExposure.load(std::memory_order_relaxed);
				if (MenuItem("Auto Exposure", nullptr, &autoExposure))
					gAutoExposure.store(autoExposure, std::memory_order_relaxed);
				SetItemTooltip("Adapts the exposure to the scene's brightness over time.");
				if (autoExposure)
					TextDisabled("Auto: %+.2f EV", gAutoExposureStops.load(std::memory_order_relaxed));
				if (BeginMenu("Tonemapper"))
				{
					static constexpr std::array<std::pair<const char*, uint32_t>, 5> kTonemappers{{
						{"Khronos PBR Neutral", TONEMAPPER_PBR_NEUTRAL},
						{"ACES", TONEMAPPER_ACES},
						{"AgX", TONEMAPPER_AGX},
						{"Reinhard", TONEMAPPER_REINHARD},
						{"Linear (clamped)", TONEMAPPER_LINEAR},
					}};
					auto current = gTonemapper.load(std::memory_order_relaxed);
					for (auto [name, tonemapper] : kTonemappers)
						if (MenuItem(name, nullptr, current == tonemapper))
							gTonemapper.store(tonemapper, std::memory_order_relaxed);
					ImGui::EndMenu();
				}
			}
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

		// the frame's exposure histogram, and instance and joint buffers, now that the frame's previous use of them is done
		// (see the fences above)
		UpdateAutoExposure(rhi, newFrameIndex);
		if (gModel && gModel->Moves())
		{
			ScenePose pose;
			ScenePose from;
			float weight = 1.0F;
			{
				auto animation = gAnimation.Write();
				auto& state = animation.Get();
				auto now = std::chrono::steady_clock::now();
				auto elapsed = std::chrono::duration<double>(now - state.last).count();
				state.last = now;
				if (state.playing)
				{
					state.time += elapsed;
					state.previousTime += elapsed;
				}
				// the fade runs in real time, also while paused
				state.fade = std::min(state.fade + elapsed, AnimationState::kCrossfade);
				pose = {.animation = state.selected, .time = static_cast<float>(state.time)};
				from = {.animation = state.previous, .time = static_cast<float>(state.previousTime)};
				weight = static_cast<float>(state.fade / AnimationState::kCrossfade);
			}
			gModel->Animate(newFrameIndex, pose, from, weight);
			ApplyPointerValues(rhi, *graphics, *gModel);
		}
		
		auto& renderImageSet = *device.GetResource<RenderImageSet>(gRenderImageSetUuids[newFrameIndex]);

		auto cmd = graphicsQueue.GetPool().Commands();

		//NOLINTBEGIN(bugprone-suspicious-stringview-data-usage)

		ZoneScopedN("WindowedApplication::Draw::submit");

		GPU_SCOPE_COLLECT(cmd, graphicsQueue);

		// empty transparency lists, once the previous frame's ComputeMain has read them
		{
			CommandEncoder encoder(cmd);
			encoder.Barrier(PipelineStage::kComputeShader, Access::kShaderRead, PipelineStage::kTransfer, Access::kTransferWrite);
			encoder.FillBuffer(*device.GetResource<Buffer>(gOitHeadsUuid), 0, 0, SHADER_TYPES_OIT_NONE);
			encoder.FillBuffer(*device.GetResource<Buffer>(gOitCounterUuid), 0, 0, 0);
			encoder.Barrier(
				PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kFragmentShader, Access::kShaderRead | Access::kShaderWrite);
		}

		DrawMainPass(rhi, window, pipeline, graphicsQueue, cmd, newFrameIndex, graphics->timeline, MainPassPhase::kOpaque);

		// transmissive materials see the opaque scene behind them: copied, with mips for their roughness
		if (gModel && HasTransmission(*gModel))
		{
			GPU_SCOPE(cmd, graphicsQueue, transmission);
			renderImageSet.Transition(cmd, ImageLayout::kTransferSource, ImageAspect::kColor, kColorAttachment);
			auto& transmissionImage = *device.GetResource<Image>(gTransmissionImageUuid);
			transmissionImage.BlitFrom(cmd, *renderImageSet.GetImage(kColorAttachment));
			transmissionImage.GenerateMips(cmd, ImageLayout::kShaderReadOnly);
			DrawMainPass(rhi, window, pipeline, graphicsQueue, cmd, newFrameIndex, graphics->timeline, MainPassPhase::kTransmissive);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, computeMain);

			// the transparency lists the main pass wrote, and the frame's exposure histogram, emptied for ComputeMain to
			// count into
			{
				CommandEncoder encoder(cmd);
				encoder.Barrier(PipelineStage::kFragmentShader, Access::kShaderWrite, PipelineStage::kComputeShader, Access::kShaderRead);
				constexpr auto kHistogramSize = SHADER_TYPES_EXPOSURE_BINS * sizeof(uint32_t);
				encoder.FillBuffer(
					*device.GetResource<Buffer>(gExposureHistogramUuid), newFrameIndex * kHistogramSize, kHistogramSize, 0);
				encoder.Barrier(
					PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite);
			}

			renderImageSet.SetLoadOp(LoadOp::kLoad, kColorAttachment);
			renderImageSet.SetStoreOp(StoreOp::kStore, kColorAttachment);
			renderImageSet.Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor, kColorAttachment);
			renderImageSet.SetLoadOp(LoadOp::kLoad, kDepthAttachment, LoadOp::kClear);
			renderImageSet.SetStoreOp(StoreOp::kStore, kDepthAttachment, StoreOp::kStore);
			renderImageSet.Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kDepth | ImageAspect::kStencil, kDepthAttachment);

			swapchain.SetLoadOp(LoadOp::kClear, 0);
			swapchain.SetStoreOp(StoreOp::kStore, 0);
			swapchain.Transition(cmd, ImageLayout::kGeneral, ImageAspect::kColor, 0);

			pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kCompute);

			pipeline.SetDescriptorData(
				"gTextures",
				ImageBinding{
					.sampler={},
					.imageView=renderImageSet.GetAttachments()[kColorAttachment],
					.layout=renderImageSet.GetLayout(kColorAttachment)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				SHADER_TYPES_RENDER_TARGET_TEXTURE_BASE + newFrameIndex);

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
			// the backdrop's: the views, the environment and its sampler; and the transparency lists
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_VIEW);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
			pipeline.BindPipelineAuto(cmd);

			PushConstants pushConstants = FramePushConstants(newFrameIndex);

			pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));

			// cover the whole swapchain image: ComputeMain has 16x16 threads per group, each copying a 16x16 pixel bucket
			constexpr uint32_t kComputePixelsPerGroup = 16U * 16U;
			auto dstExtent = swapchain.GetExtent();
			CommandEncoder(cmd).Dispatch(
				(dstExtent.width + kComputePixelsPerGroup - 1) / kComputePixelsPerGroup,
				(dstExtent.height + kComputePixelsPerGroup - 1) / kComputePixelsPerGroup,
				1U);
			// for the cpu to read the histogram once the frame is done (see UpdateAutoExposure)
			CommandEncoder(cmd).Barrier(PipelineStage::kComputeShader, Access::kShaderWrite, PipelineStage::kHost, Access::kHostRead);
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
	// the default, and the clamping one (SHADER_TYPES_CLAMP_SAMPLER)
	std::vector<SamplerDesc> samplerDescs{
		SamplerDesc{.maxAnisotropy = kDefaultSamplerMaxAnisotropy},
		SamplerDesc{
			.addressModeU = AddressMode::kClampToEdge, .addressModeV = AddressMode::kClampToEdge, .addressModeW = AddressMode::kClampToEdge}};
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
		std::vector<MaterialData> materialData(SHADER_TYPES_MATERIAL_COUNT, DefaultMaterialData());

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

		// filled in by InstallModel and InstallImage
		std::vector<TextureView> textureViewData(SHADER_TYPES_TEXTURE_VIEW_COUNT);
		core::TaskCreateInfo<void> textureViewTransfersDone;
		auto textureViews = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("TextureViews"),
				SHADER_TYPES_TEXTURE_VIEW_COUNT * sizeof(TextureView),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible},
			textureViewData.data(),
			cmd,
			textureViewTransfersDone);
		gTextureViewsUuid = textureViews->GetUuid();
		timelineCallbacks.emplace_back(textureViewTransfersDone.handle);

		// bound until a model is installed, which binds its own instance buffer (see Model::GetInstanceBuffer)
		constexpr uint32_t kMatrix4x4ElementCount = 16;
		std::vector<ModelInstance> modelInstances(1);
		static const auto kIdentityMatrix = glm::mat4x4(1.0);
		std::copy_n(&kIdentityMatrix[0][0], kMatrix4x4ElementCount, &modelInstances[0].modelTransform[0][0]);
		std::copy_n(&kIdentityMatrix[0][0], kMatrix4x4ElementCount, &modelInstances[0].inverseTransposeModelTransform[0][0]);

		core::TaskCreateInfo<void> modelTransfersDone;
		auto modelInstancesBuffer = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("ModelInstances"),
				modelInstances.size() * sizeof(ModelInstance),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible
			},
			modelInstances.data(),
			cmd,
			modelTransfersDone);
		gModelInstancesUuid = modelInstancesBuffer->GetUuid();
		timelineCallbacks.emplace_back(modelTransfersDone.handle);

		// written when a model is installed (see UpdateLights)
		std::vector<LightData> lightData(SHADER_TYPES_LIGHT_COUNT);
		core::TaskCreateInfo<void> lightTransfersDone;
		auto lights = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("Lights"),
				lightData.size() * sizeof(LightData),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible},
			lightData.data(),
			cmd,
			lightTransfersDone);
		gLightsUuid = lights->GetUuid();
		timelineCallbacks.emplace_back(lightTransfersDone.handle);

		// dark (and sampling the black texture) until an environment is installed (see InstallEnvironment)
		std::array<EnvironmentData, 1> environmentData{
			EnvironmentData{
				.textureId = SHADER_TYPES_ENVIRONMENT_TEXTURE, .samplerId = kDefaultSamplerId, .levelCount = 1.0F, .sheenLevelCount = 1.0F}};
		core::TaskCreateInfo<void> environmentTransfersDone;
		auto environmentBuffer = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("Environment"),
				sizeof(environmentData),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible},
			environmentData.data(),
			cmd,
			environmentTransfersDone);
		gEnvironmentUuid = environmentBuffer->GetUuid();
		timelineCallbacks.emplace_back(environmentTransfersDone.handle);

		// counted into by ComputeMain, read by UpdateAutoExposure
		std::vector<uint32_t> histograms(static_cast<size_t>(SHADER_TYPES_FRAME_COUNT) * SHADER_TYPES_EXPOSURE_BINS);
		core::TaskCreateInfo<void> histogramTransfersDone;
		auto histogramBuffer = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("Exposure Histogram"),
				histograms.size() * sizeof(uint32_t),
				BufferUsage::kStorage | BufferUsage::kTransferDestination,
				MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent},
			histograms.data(),
			cmd,
			histogramTransfersDone);
		gExposureHistogramUuid = histogramBuffer->GetUuid();
		timelineCallbacks.emplace_back(histogramTransfersDone.handle);

		std::array<SkinVertex, 1> defaultSkinVertices{};
		core::TaskCreateInfo<void> skinTransfersDone;
		auto skinVertices = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("DefaultSkinVertices"),
				sizeof(defaultSkinVertices),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible},
			defaultSkinVertices.data(),
			cmd,
			skinTransfersDone);
		gDefaultSkinVerticesUuid = skinVertices->GetUuid();
		timelineCallbacks.emplace_back(skinTransfersDone.handle);

		std::array<std::array<float, 16>, 1> defaultJoints{gfx::mesh::kIdentityTransform};
		core::TaskCreateInfo<void> jointTransfersDone;
		auto joints = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("DefaultJoints"),
				sizeof(defaultJoints),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible},
			defaultJoints.data(),
			cmd,
			jointTransfersDone);
		gDefaultJointsUuid = joints->GetUuid();
		timelineCallbacks.emplace_back(jointTransfersDone.handle);

		std::array<MorphDelta, 1> defaultMorphDeltas{};
		core::TaskCreateInfo<void> morphDeltaTransfersDone;
		auto morphDeltas = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("DefaultMorphDeltas"),
				sizeof(defaultMorphDeltas),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible},
			defaultMorphDeltas.data(),
			cmd,
			morphDeltaTransfersDone);
		gDefaultMorphDeltasUuid = morphDeltas->GetUuid();
		timelineCallbacks.emplace_back(morphDeltaTransfersDone.handle);

		std::array<float, 1> defaultMorphWeights{};
		core::TaskCreateInfo<void> morphWeightTransfersDone;
		auto morphWeights = device.CreateResource<Buffer>(
			BufferCreateDesc{
				device.CreateDeviceObjectCreateDesc("DefaultMorphWeights"),
				sizeof(defaultMorphWeights),
				BufferUsage::kStorage,
				MemoryProperty::kHostVisible},
			defaultMorphWeights.data(),
			cmd,
			morphWeightTransfersDone);
		gDefaultMorphWeightsUuid = morphWeights->GetUuid();
		timelineCallbacks.emplace_back(morphWeightTransfersDone.handle);

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

	for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
	{
		pipeline.SetDescriptorData(
			"gModelInstances",
			BufferBinding{.buffer = *device.GetResource<Buffer>(gModelInstancesUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
		pipeline.SetDescriptorData(
			"gJointMatrices",
			BufferBinding{.buffer = *device.GetResource<Buffer>(gDefaultJointsUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
		pipeline.SetDescriptorData(
			"gMorphWeights",
			BufferBinding{.buffer = *device.GetResource<Buffer>(gDefaultMorphWeightsUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
	}
	pipeline.SetDescriptorData(
		"gSkinVertices",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gDefaultSkinVerticesUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
	pipeline.SetDescriptorData(
		"gMorphDeltas",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gDefaultMorphDeltasUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

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
					{"FragmentTransparent", SLANG_STAGE_FRAGMENT}, // see kTransparentFragmentShader
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
		"gLights",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gLightsUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

	pipeline.SetDescriptorData(
		"gEnvironment",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gEnvironmentUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
	pipeline.SetDescriptorData(
		"gTextures",
		ImageBinding{.sampler = {}, .imageView = *device.GetResource<ImageView>(gBlackTextureViewUuid), .layout = ImageLayout::kShaderReadOnly},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
		SHADER_TYPES_ENVIRONMENT_TEXTURE);
	pipeline.SetDescriptorData(
		"gTextures",
		ImageBinding{.sampler = {}, .imageView = *device.GetResource<ImageView>(gBlackTextureViewUuid), .layout = ImageLayout::kShaderReadOnly},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
		SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE);

	pipeline.SetDescriptorData(
		"gTextureViews",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gTextureViewsUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_MATERIAL);

	for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
	{
		pipeline.SetDescriptorData(
			"gModelInstances",
			BufferBinding{.buffer = *device.GetResource<Buffer>(gModelInstancesUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
		pipeline.SetDescriptorData(
			"gJointMatrices",
			BufferBinding{.buffer = *device.GetResource<Buffer>(gDefaultJointsUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
		pipeline.SetDescriptorData(
			"gMorphWeights",
			BufferBinding{.buffer = *device.GetResource<Buffer>(gDefaultMorphWeightsUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
	}
	pipeline.SetDescriptorData(
		"gSkinVertices",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gDefaultSkinVerticesUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
	pipeline.SetDescriptorData(
		"gMorphDeltas",
		BufferBinding{.buffer = *device.GetResource<Buffer>(gDefaultMorphDeltasUuid), .offset = 0},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

	pipeline.SetDescriptorData(
		"gSamplers",
		ImageBinding{.sampler = (*device.GetResource<SamplerVector>(gSamplersUuid))[0]},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
		kDefaultSamplerId);
	pipeline.SetDescriptorData(
		"gSamplers",
		ImageBinding{.sampler = (*device.GetResource<SamplerVector>(gSamplersUuid))[1]},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
		SHADER_TYPES_CLAMP_SAMPLER);

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
