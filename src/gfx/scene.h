#pragma once

#include <gfx/environment.h>
#include <gfx/gpu.h>
#include <gfx/indirectlight.h>
#include <gfx/meshimport.h>
#include <gfx/model.h>
#include <gfx/renderoutput.h>
#include <gfx/shaders/capi.h>
#include <gfx/upscaling.h>

#include <core/concurrentaccess.h>
#include <core/std_extra.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core
{
class TaskExecutor;
} // namespace core

namespace gfx
{

class Renderer;
class Views;

// what is drawn, and how it is lit: the installed model (with its materials, textures, animation and lights), image
// (on the default material) and environment (and its dominant lights), the default resources standing in for what
// they don't have, and the renderer drawing them through the views. loads run in load tasks and install on the draw
// thread (through RHI::drawCalls); the ui reads and changes the settings and the animation.
class Scene final
{
public:
	// set from the ui, read by the draw thread
	struct Settings
	{
		// whether the dominant light merges with a model's light from the same direction (View > Environment,
		// SPEEDO_MERGE_LIGHTS=0 to start without)
		std::atomic<bool> mergeLights = true;
		// whether the lights cast shadows (View > Shadows, SPEEDO_SHADOWS=0 to start without)
		std::atomic<bool> shadows = true;
		// whether the brightest lights' shadows are traced through the model's BVHs (FrameInputs::tracedShadows; View >
		// Ray Traced Shadows, SPEEDO_TRACED_SHADOWS=0 to start with the atlas's alone)
		std::atomic<bool> tracedShadows = true;
		// and at every pixel rather than where the atlas isn't sure (SPEEDO_TRACED_SHADOWS=all, for comparisons)
		std::atomic<bool> tracedShadowsEverywhere = false;
		// whether emissive surfaces light the scene as lights do (FrameInputs::emissiveLights; View > Emissive Lights,
		// SPEEDO_EMISSIVE_LIGHTS=0 to start without: the indirect lighting's rays then take their emission as they meet them)
		std::atomic<bool> emissiveLights = true;
		// whether the views skip the draws outside their frustum (View > Culling, SPEEDO_CULLING=0 to start without)
		std::atomic<bool> culling = true;
		// whether each fragment loops over only the lights that may reach its cluster (View > Clustered Lights,
		// SPEEDO_CLUSTERED_LIGHTS=0 to start without: over all of them)
		std::atomic<bool> clusteredLights = true;
		// whether the renderer keeps a copy of each frame's lights and shadow views (Renderer::GetLightCapture), for the
		// ui's light gizmos (View > Lights)
		std::atomic<bool> captureLights = false;
		// the final image is scaled by 2^exposureStops before tonemapping (with auto exposure, on top of what it picks:
		// View > Auto Exposure, on by default since 2026-10-26, the user's call with the sky's time of day; SPEEDO_AUTO_EXPOSURE=0
		// to start without, as captures compared at a fixed exposure need)
		std::atomic<float> exposureStops = 0.0F;
		std::atomic<bool> autoExposure = true;
		// the tonemapper (TONEMAPPER_*)
		std::atomic<uint32_t> tonemapper = TONEMAPPER_PBR_NEUTRAL;
		// what the views show (DEBUG_VIEW_*: the light, or a debug view of the surfaces or the lighting; View > Debug
		// View, SPEEDO_DEBUG_VIEW)
		std::atomic<uint32_t> debugView = DEBUG_VIEW_NONE;
		// whether the indirect lighting runs (see FrameInputs::indirectLighting; View > Indirect Lighting; on by default
		// since 2026-10-22, SPEEDO_GI=0 to start without it)
		std::atomic<bool> indirectLighting = true;
		// whether its rays trace the model's triangles (its BVHs, see gfx/bvh.h) or walk the voxels (FrameInputs::traceTriangles;
		// View > Indirect Lighting Rays; triangles by default, SPEEDO_GI_TRACE=voxels to start with the voxels)
		std::atomic<bool> traceTriangles = true;
		// the indirect lighting's voxels along the scene's longest axis (FrameInputs::giResolution; View > Indirect Lighting
		// Resolution, SPEEDO_GI_RESOLUTION to start with another): 128 by default, finer for light through narrow openings
		std::atomic<uint32_t> giResolution = gi::kDefaultResolution;
		// whether the surface cache refines the probes' light (FrameInputs::giSurfaceCache; View > Indirect Lighting Surface
		// Cache; on by default, SPEEDO_GI_CACHE=0 to start without it)
		std::atomic<bool> giSurfaceCache = true;
		// whether the shading takes the near field's rays near other surfaces (FrameInputs::giNearField; View > Indirect
		// Lighting Near Field; on by default, SPEEDO_GI_NEAR=0 to start without it)
		std::atomic<bool> giNearField = true;
		// and how near what its rays meet is a contact, in voxels (FrameInputs::giNearContact; View > Indirect Lighting Near
		// Field Contact, SPEEDO_GI_NEAR_CONTACT to start with another)
		std::atomic<float> giNearContact = gi::kDefaultNearContact;
		// and every how many pixels each way its rays are traced (FrameInputs::giNearStride: 2, a quarter of the pixels, or 4, a
		// sixteenth; View > Indirect Lighting Near Field Rays, SPEEDO_GI_NEAR_STRIDE to start with another)
		std::atomic<uint32_t> giNearStride = 2;
		// and whether the near field is accumulated over frames (FrameInputs::giNearTemporal; View > Indirect Lighting Near
		// Field Over Frames, SPEEDO_GI_NEAR_TEMPORAL=0 to start without)
		std::atomic<bool> giNearTemporal = true;
		// temporal upscaling (FrameInputs::upscaling; View > Upscaling, SPEEDO_UPSCALING): a change resizes what is sized
		// after the output, before the next frame (see NeedsResize)
		std::atomic<Upscaling> upscaling = Upscaling::kOff;
		// the environment's intensity (radiance scale) and rotation about +y (degrees), and whether it is drawn as the
		// backdrop (see PushConstants::environmentIntensity)
		std::atomic<float> environmentIntensity = 1.0F;
		std::atomic<float> environmentRotationDegrees = 0.0F;
		std::atomic<bool> environmentBackdrop = true;
		// the sky's time of day (hours of local solar time, 0 to 24), latitude (degrees, north positive) and day of the
		// year, which put its sun (see environment::SunDirection; View > Environment, SPEEDO_TIME_OF_DAY, SPEEDO_LATITUDE,
		// SPEEDO_DAY_OF_YEAR), and whether the time advances, and how fast, in multiples of real time (View > Environment >
		// Advance Time and Time Speed, SPEEDO_TIME_SPEED); the draw thread advances it
		std::atomic<float> timeOfDay = environment::kSkyDefaultTimeOfDay;
		std::atomic<float> latitude = environment::kSkyDefaultLatitude;
		std::atomic<float> dayOfYear = environment::kSkyDefaultDayOfYear;
		std::atomic<bool> advanceTime = false;
		std::atomic<float> timeSpeed = 60.0F;
	};

	// which of the installed model's animations plays (see Model::Animate), and its clock. the ui thread reads and
	// changes it, the draw thread advances it.
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

	// the installed model's file, its scenes and its material variants (a gltf file's, see ModelDesc::scenes and
	// variants), for View > Scene and View > Variant. the draw thread sets it, the ui thread reads it.
	struct SceneState
	{
		std::string filePath;
		std::vector<std::string> names;
		uint32_t current = 0;
		std::vector<std::string> variants;
		std::optional<uint32_t> variant; // none: the default materials
	};

	// one of the installed environment's dominant lights, for the ui: its irradiance (lux, luminance) and whether the
	// model has the same light
	struct DominantLightInfo
	{
		float lux = 0.0F;
		bool merged = false;

		bool operator==(const DominantLightInfo&) const = default;
	};

	// what a frame is drawn into, and with (see Record). its callables are called during Record
	struct FrameTarget
	{
		// the frame's slot of the per frame buffers, whose last frame the gpu is done with (see SHADER_TYPES_FRAME_COUNT),
		// and the output's image it is drawn into (a window's swapchain image: see RenderOutput)
		uint16_t frameIndex = 0;
		uint16_t outputIndex = 0;
		Queue* graphicsQueue = nullptr;
		uint64_t graphicsTimeline = 0;
		// record the ui's texture uploads and draws, over a window's frames (see SwapchainOutput)
		core::std_extra::function_ref<void(CommandBufferHandle cmd)> prepareUi = NoUi;
		core::std_extra::function_ref<void(CommandBufferHandle cmd)> drawUi = NoUi;
		// the ui's frosted panels, whose backdrop is blurred (see UiBlurRect: their rectangles and corners, in pixels of the
		// output; the threads are the renderer's to fill in). of the ui frame drawUi draws
		std::span<const UiBlurRect> uiBlurRects;
		// runs tasks in the background (pipelines created there), if given
		core::TaskExecutor* executor = nullptr;
	};

	// creates the default resources and the pipeline layouts, binds them, and sizes the renderer after output. draws with
	// pipeline (its descriptor sets hold the scene's bindings, so each scene needs its own), into views' viewports, and
	// into output (the window's swapchain, or an image: see RenderOutput). views and output must outlive the scene.
	Scene(RHI& rhi, Pipeline& pipeline, Views& views, RenderOutput& output);
	~Scene();

	Scene(const Scene&) = delete;
	Scene& operator=(const Scene&) = delete;

	// what the pipeline's descriptor sets are allocated from (see RHIInitializationData::descriptorPoolSizes)
	[[nodiscard]] static std::vector<rhi::DescriptorPoolSize> DescriptorPoolSizes();

	// load, and have the draw thread install, unless the load is cancelled. call from a load task (see core::LoadQueue).
	// a model, or several side by side as one (see Model::Load), with its materials' textures. options: a gltf file's
	// scene and material variant to load (one file only), else its default ones
	// several files are arranged as arrangement says (see ModelArrangement). returns the name of the model installed (see
	// SceneState::filePath: a file's path, or for several their directory and count), or nothing if the load failed or was
	// cancelled: it is installed once the draw thread has run the install
	std::optional<std::string> LoadModels(
		const std::vector<std::string>& filePaths,
		std::atomic_uint8_t& progress,
		const mesh::ImportOptions& options = {},
		ModelArrangement arrangement = ModelArrangement::kSideBySide);
	// an image, on the default material (material 0)
	void LoadImage(std::string_view filePath, std::atomic_uint8_t& progress);
	// an environment panorama (or without one, the sky)
	void LoadEnvironment(std::optional<std::string> filePath, std::atomic_uint8_t& progress);

	// resizes what is sized after the output (its render targets), after the output was (re)created (the window's
	// swapchain, or ImageOutput::Resize). call with the gpu idle, holding the draw lock.
	void Resize();
	// whether the render targets need resizing for a change of Settings::upscaling (the render size): call Resize, as for
	// the output's, before the next frame. draw thread.
	[[nodiscard]] bool NeedsResize() const noexcept;

	// updates the frame's buffers for the frame about to be recorded, after its fence: the exposure histogram it read,
	// the animation and what it moves, and the lights. draw thread.
	void Update(QueueTimelineContextData& graphics, uint32_t frameIndex);

	// records the frame into cmd (see Renderer::Record). draw thread.
	void Record(CommandBufferHandle cmd, const FrameTarget& target);

	[[nodiscard]] Settings& GetSettings() noexcept;
	[[nodiscard]] core::ConcurrentAccess<AnimationState>& GetAnimation() noexcept;
	[[nodiscard]] core::ConcurrentAccess<SceneState>& GetScenes() noexcept;
	// whether the installed environment is the sky (which the time of day moves the sun of, see Settings::timeOfDay)
	[[nodiscard]] bool IsSkyInstalled() const noexcept;
	[[nodiscard]] core::ConcurrentAccess<std::string>& GetEnvironmentName() noexcept;
	[[nodiscard]] core::ConcurrentAccess<std::vector<DominantLightInfo>>& GetEnvironmentLightInfo() noexcept;
	// the installed model's lights' sizes the ui set (LightData::size), by light: a directional light's angular radius in
	// degrees, a point or spot light's radius; NaN (or past the end) for the default, the sun's for a directional light
	// (environment::kSkySunRadius), 0 for a point or spot light (glTF's lights are punctual). cleared when a model is
	// installed
	[[nodiscard]] core::ConcurrentAccess<std::vector<float>>& GetLightSizes() noexcept;
	// the exposure auto exposure picked, in stops (see Settings::autoExposure)
	[[nodiscard]] float GetAutoExposureStops() const noexcept;
	// the bytes of device memory the renderer's transients take, and would take without aliasing
	[[nodiscard]] uint64_t GetTransientMemory() const noexcept;
	[[nodiscard]] uint64_t GetUnaliasedTransientMemory() const noexcept;
	[[nodiscard]] const Renderer& GetRenderer() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> myImpl;
};

} // namespace gfx
