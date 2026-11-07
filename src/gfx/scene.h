#pragma once

#include <gfx/environment.h>
#include <gfx/gpu.h>
#include <gfx/indirectlight.h>
#include <gfx/meshimport.h>
#include <gfx/model.h>
#include <gfx/physicalcamera.h>
#include <gfx/renderoutput.h>
#include <gfx/shaders/capi.h>
#include <gfx/upscaling.h>

#include <core/cancelled.h>
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
#include <utility>
#include <vector>

namespace core
{
class TaskExecutor;
} // namespace core

namespace gfx
{

// how the camera's autofocus picks what to focus at (see Scene::Settings::focusMode), as a system camera's modes:
// a single point at the view's center, a zone around it, the whole view (the nearest subject), or by hand
enum class FocusMode : uint8_t
{
	kSpot,
	kZone,
	kWide,
	kManual,
};

[[nodiscard]] constexpr std::string_view FocusModeName(FocusMode mode) noexcept
{
	switch (mode)
	{
	case FocusMode::kZone: return "zone";
	case FocusMode::kWide: return "wide";
	case FocusMode::kManual: return "manual";
	case FocusMode::kSpot:
	default: return "spot";
	}
}

[[nodiscard]] constexpr std::optional<FocusMode> ParseFocusMode(std::string_view text) noexcept
{
	for (auto mode : {FocusMode::kSpot, FocusMode::kZone, FocusMode::kWide, FocusMode::kManual})
		if (text == FocusModeName(mode))
			return mode;
	return std::nullopt;
}

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
		// whether the smooth surfaces' reflections and the transmissive ones' refractions are traced, where the device has
		// ray queries (FrameInputs::tracedReflections and tracedRefractions; View > Ray Traced Reflections and Ray Traced
		// Refractions, SPEEDO_TRACED_REFLECTIONS=0 and SPEEDO_TRACED_REFRACTIONS=0 to start without)
		std::atomic<bool> tracedReflections = true;
		std::atomic<bool> tracedRefractions = true;
		// whether the transmissive surfaces refract the lights' light into caustics, where the device has ray queries
		// (FrameInputs::caustics; View > Caustics, SPEEDO_CAUSTICS=0 to start without: they tint it as it goes straight through)
		std::atomic<bool> caustics = true;
		// whether emissive surfaces light the scene as lights do (FrameInputs::emissiveLights; View > Emissive Lights,
		// SPEEDO_EMISSIVE_LIGHTS=0 to start without: the indirect lighting's rays then take their emission as they meet them)
		std::atomic<bool> emissiveLights = true;
		// whether the models loaded from now on get their BVHs (see ModelBvh), which the traced shadows, the emitters' light and the
		// indirect lighting's rays need (the renderer leaves those out without them): off for a scene that traces no rays
		// (the file browser's preview)
		std::atomic<bool> modelBvhs = true;
		// whether the views skip the draws outside their frustum (View > Culling, SPEEDO_CULLING=0 to start without)
		std::atomic<bool> culling = true;
		// whether each fragment loops over only the lights that may reach its cluster (View > Clustered Lights,
		// SPEEDO_CLUSTERED_LIGHTS=0 to start without: over all of them)
		std::atomic<bool> clusteredLights = true;
		// whether the renderer keeps a copy of each frame's lights and shadow views (Renderer::GetLightCapture), for the
		// ui's light gizmos (View > Lights)
		std::atomic<bool> captureLights = false;
		// the camera's exposure (see gfx/physicalcamera.h): its mode (camera::ExposureMode; View > Auto Exposure is program
		// against manual, View > Camera > Exposure the priorities; SPEEDO_EXPOSURE_MODE, SPEEDO_AUTO_EXPOSURE=0 for manual;
		// auto exposure on by default since 2026-10-26, the user's call with the sky's time of day, and a program mode since
		// 2026-11-05: the camera chooses its aperture, shutter and ISO for the exposure auto exposure asks for, see
		// camera::Choose) and what is set (the aperture, shutter and ISO: all of them in manual, one of them pinned in a
		// priority mode, none in program; SPEEDO_APERTURE, SPEEDO_SHUTTER, SPEEDO_ISO; sunny sixteen by default, which
		// exposes a unit of radiance as white, the renderer's 0 stops: captures compared at a fixed exposure take
		// SPEEDO_AUTO_EXPOSURE=0 as before); and the final image is scaled by 2^exposureStops more (View > Exposure: an
		// exposure compensation)
		std::atomic<float> exposureStops = 0.0F;
		std::atomic<uint8_t> exposureMode = static_cast<uint8_t>(camera::ExposureMode::kProgram);
		std::atomic<float> aperture = camera::Exposure{}.aperture;
		std::atomic<float> shutter = camera::Exposure{}.shutter;
		std::atomic<float> iso = camera::Exposure{}.iso;
		[[nodiscard]] bool AutoExposure() const noexcept { return exposureMode.load(std::memory_order_relaxed) != static_cast<uint8_t>(camera::ExposureMode::kManual); }
		// the camera's lens (see gfx/lens.h: an index into lens::Lenses(), 0 the pinhole, which shows the frame as it is;
		// View > Camera > Lens, SPEEDO_LENS=<key>): a prime pins the focal length (the views', see Views::SetLens) and the
		// aperture's range (see GetExposureLimits), and the Lens pass draws what it does to the frame (its vignetting, its
		// lateral chromatic aberration and its distortion, see LensData; the glare follows)
		std::atomic<uint32_t> lens = 0;
		// and whether its glare is drawn (the bloom it veils the frame with, and the ghosts and starbursts of the bright
		// blobs the frame holds, see lens.slang and flare.slang; View > Camera > Lens Glare, SPEEDO_LENS_GLARE=0 without)
		std::atomic<bool> lensGlare = true;
		// and what it focuses at (the depth of field, see dof.slang; a real lens's, the pinhole's everything), as a system
		// camera's autofocus modes have it (FocusMode; View > Camera > Focus, SPEEDO_FOCUS=spot|zone|wide|<meters>, which
		// Copy Pose writes): what is under the view's center spot (SHADER_TYPES_FOCUS_SPOT of the view each way, a single
		// point's), its center zone (SHADER_TYPES_FOCUS_ZONE), the nearest thing anywhere in the view (wide: the nearest
		// tenth of its pixels, as an auto area picks the nearest subject), or focusDistance, meters (manual); and whether
		// the autofocus follows continuously (AF-C) or holds what it last acquired (AF-S: View > Camera > Focus > Focus
		// Now acquires again, SPEEDO_FOCUS_CONTINUOUS=0 holds the first)
		std::atomic<uint8_t> focusMode = static_cast<uint8_t>(FocusMode::kSpot);
		std::atomic<float> focusDistance = 2.0F;
		std::atomic<bool> focusContinuous = true;
		std::atomic<bool> focusAcquire = false; // a request (Focus Now), taken by the draw thread
		// and whether what moves while its shutter is open smears (the motion blur, see motionblur.slang; View > Camera >
		// Motion Blur, SPEEDO_MOTION_BLUR=0 without) and whether its sensor's shot noise at the ISO shows (the grain, see
		// SharpenLens; View > Camera > Grain, SPEEDO_GRAIN=0 without), each with a real lens, which Copy Pose writes
		std::atomic<bool> motionBlur = true;
		std::atomic<bool> grain = true;
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
		// sixteenth, gi::kDefaultNearStride; View > Indirect Lighting Near Field Rays, SPEEDO_GI_NEAR_STRIDE to start with another)
		std::atomic<uint32_t> giNearStride = gi::kDefaultNearStride;
		// and whether the near field is accumulated over frames (FrameInputs::giNearTemporal; View > Indirect Lighting Near
		// Field Over Frames, SPEEDO_GI_NEAR_TEMPORAL=0 to start without)
		std::atomic<bool> giNearTemporal = true;
		// temporal upscaling (FrameInputs::upscaling; View > Upscaling, SPEEDO_UPSCALING; balanced by default, kDefaultUpscaling): a
		// change resizes what is sized after the output, before the next frame (see NeedsResize)
		std::atomic<Upscaling> upscaling = kDefaultUpscaling;
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
		// and the year, which the moon's place and phase go by (and the stars' a little): this one by default, the user's
		// call (View > Environment > Year, SPEEDO_YEAR, which Copy Pose always writes)
		std::atomic<int> year = environment::CurrentYear();
		std::atomic<bool> advanceTime = false;
		std::atomic<float> timeSpeed = 60.0F;
		// how clear the sky's air is: how far one sees through it, km (see environment::SkyAerosols; View > Environment >
		// Visibility, SPEEDO_VISIBILITY): the sky, its sun's light and the aerial perspective go by it
		std::atomic<float> visibility = environment::kSkyDefaultVisibility;
		// the aerial perspective, where the environment is the sky (FrameInputs::aerialPerspective; View > Environment >
		// Aerial Perspective, SPEEDO_AERIAL_PERSPECTIVE=0 to start without), and its light shafts, the sun's shadows in the air
		// (View > Environment > Light Shafts, SPEEDO_LIGHT_SHAFTS=0); and the scene's unit in meters (see gfx::SceneUnit;
		// View > Units, SPEEDO_UNITS or SPEEDO_METERS_PER_UNIT), which the air's thickness, the lights' falloff and the
		// ground plane's checker go by
		std::atomic<bool> aerialPerspective = true;
		std::atomic<bool> lightShafts = true;
		std::atomic<float> metersPerUnit = 1.0F;
		// the fog near the ground, in the aerial perspective's air (see AerialPerspectiveData::fog; View > Environment > Fog,
		// SPEEDO_FOG=0 to start without), its visibility at the ground (km; Fog Visibility, SPEEDO_FOG_VISIBILITY) and the
		// scale height its density falls by (m; Fog Height, SPEEDO_FOG_HEIGHT): a clear day's by default
		std::atomic<bool> fog = true;
		std::atomic<float> fogVisibility = environment::kFogDefaultVisibility;
		std::atomic<float> fogHeight = environment::kFogDefaultHeight;
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
	// variants), for View > Scene, Copy Pose and Paste Pose. the draw thread sets it, the ui thread reads it.
	struct SceneState
	{
		std::string filePath;
		// the files it was loaded from (one, or several as one, see Model::Load) and how several are arranged: what loads
		// it again (Copy Pose writes them)
		std::vector<std::string> filePaths;
		ModelArrangement arrangement = ModelArrangement::kSideBySide;
		std::vector<std::string> names;
		uint32_t current = 0;
		std::vector<std::string> variants;
		std::optional<uint32_t> variant; // none: the default materials
		ModelGround ground = ModelGround::kNone; // what it stands on (see Model::Load)
		// where files' variants are laid out to compare (see LoadVariants): the variant of each of filePaths (none: the
		// default materials), empty otherwise
		std::vector<std::optional<uint32_t>> layout;
		Bounds3f bounds; // the model's, without its ground plane (Model::GetFramedBounds): what its unit is guessed by
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
		// record the ui's texture uploads and draws, over a window's frames (see SwapchainOutput: in target, the frame begun)
		core::std_extra::function_ref<void(CommandBufferHandle cmd)> prepareUi = NoUi;
		core::std_extra::function_ref<void(CommandBufferHandle cmd, IRenderTarget& target)> drawUi = NoUiDraw;
		// the ui's frosted panels, whose backdrop is blurred (see UiBlurRect: their rectangles and corners, in pixels of the
		// output; the threads are the renderer's to fill in). of the ui frame drawUi draws
		std::span<const UiBlurRect> uiBlurRects;
		// runs tasks in the background (pipelines created there), if given
		core::TaskExecutor* executor = nullptr;
	};

	// creates the default resources and starts compiling the shaders on the application's executor (tens of seconds on a
	// cold cache, see ShaderLoader), from which Prepare makes the pipeline layouts, binds them, and sizes the renderer
	// after output. draws with pipeline (its descriptor sets hold the scene's bindings, so each scene needs its own), into
	// views' viewports, and into output (the window's swapchain, or an image: see RenderOutput). views and output must
	// outlive the scene.
	Scene(RHI& rhi, Pipeline& pipeline, Views& views, RenderOutput& output);
	~Scene();

	Scene(const Scene&) = delete;
	Scene& operator=(const Scene&) = delete;

	// what the pipeline's descriptor sets are allocated from (see RHIInitializationData::descriptorPoolSizes)
	[[nodiscard]] static std::vector<rhi::DescriptorPoolSize> DescriptorPoolSizes();

	// load, and have the draw thread install, unless the load is cancelled (the application exits, or cancelled says so:
	// it is checked where the load takes long, and before the install). call from a load task (see core::LoadQueue).
	// a model, or several side by side as one (see Model::Load), with its materials' textures. options: a gltf file's
	// scene and material variant to load (one file only), else its default ones
	// several files are arranged as arrangement says (see ModelArrangement), and stand on ground (see ModelGround). returns the name of the model installed (see
	// SceneState::filePath: a file's path, or for several their directory and count), or nothing if the load failed or was
	// cancelled: it is installed once the draw thread has run the install
	std::optional<std::string> LoadModels(
		const std::vector<std::string>& filePaths,
		std::atomic_uint8_t& progress,
		const mesh::ImportOptions& options = {},
		ModelArrangement arrangement = ModelArrangement::kSideBySide,
		ModelGround ground = ModelGround::kNone,
		core::CancelledFn cancelled = core::NeverCancelled);
	// files' material variants (KHR_materials_variants) laid out to compare: copies of files (filePaths, a file's
	// together), each with its variant (variants, as many: none, the default materials), in scene (none: each file's
	// default, see mesh::ImportOptions::scene), arranged as arrangement says (one behind the other by default, the user's
	// call: each file's copies in a column, see ModelArrangement::kBehindEachOther), standing on ground; one copy alone
	// loads its file with its variant. as LoadModels otherwise
	std::optional<std::string> LoadVariants(
		std::span<const std::string> filePaths,
		std::span<const std::optional<uint32_t>> variants,
		std::optional<size_t> scene,
		std::atomic_uint8_t& progress,
		ModelArrangement arrangement = ModelArrangement::kBehindEachOther,
		ModelGround ground = ModelGround::kNone,
		core::CancelledFn cancelled = core::NeverCancelled);
	// an image, on the default material (material 0)
	void LoadImage(std::string_view filePath, std::atomic_uint8_t& progress);
	// an environment panorama (or without one, the sky)
	void LoadEnvironment(std::optional<std::string> filePath, std::atomic_uint8_t& progress, core::CancelledFn cancelled = core::NeverCancelled);

	// makes the rest once the shaders have compiled (the layouts, their bindings, the environment filter and the renderer,
	// sized after the output), and creates its compute pipelines on the executor: ready once they are all made. call before
	// each frame until it returns true, on the draw thread, holding the draw lock and outside the graphics queue's (what
	// it makes submits to the queue). until then the scene isn't ready: it draws the output cleared with the ui over it
	// (Record), its loads import but wait to install (LoadModels and the others), and it has no renderer
	bool Prepare();
	[[nodiscard]] bool IsReady() const noexcept;
	// how far the shaders' compile is, 0 to 1: the layouts done, and an estimate for those compiling (by how long the
	// last cold compile took)
	[[nodiscard]] float GetShaderProgress() const noexcept;
	// and then the compute pipelines' creation, before the first frame (see Pipeline::RequestComputePipelines): how many
	// are done, of how many (0 of 0 until the compile is done)
	[[nodiscard]] std::pair<uint32_t, uint32_t> GetPipelineProgress() const noexcept;

	// resizes what is sized after the output (its render targets), after the output was (re)created (the window's
	// swapchain, or ImageOutput::Resize). call with the gpu idle, holding the draw lock.
	void Resize();
	// whether the render targets need resizing for a change of Settings::upscaling (the render size): call Resize, as for
	// the output's, before the next frame. draw thread.
	[[nodiscard]] bool NeedsResize() const noexcept;

	// updates the frame's buffers for the frame about to be recorded, after its fence: the exposure histogram it read,
	// the animation and what it moves, and the lights (nothing until ready). draw thread.
	void Update(QueueTimelineContextData& graphics, uint32_t frameIndex);

	// records the frame into cmd (see Renderer::Record; until ready, the output cleared and its final passes). draw thread.
	void Record(CommandBufferHandle cmd, const FrameTarget& target);

	[[nodiscard]] Settings& GetSettings() noexcept;
	[[nodiscard]] core::ConcurrentAccess<AnimationState>& GetAnimation() noexcept;
	[[nodiscard]] core::ConcurrentAccess<SceneState>& GetScenes() noexcept;
	// whether the installed environment is the sky (which the time of day moves the sun of, see Settings::timeOfDay)
	[[nodiscard]] bool IsSkyInstalled() const noexcept;
	// whether the shaders trace through the device's ray queries (DeviceLimits::rayQuery): what the traced reflections and
	// refractions need (Settings::tracedReflections)
	[[nodiscard]] bool HasRayQueries() const noexcept;
	[[nodiscard]] core::ConcurrentAccess<std::string>& GetEnvironmentName() noexcept;
	// and its file (empty for the sky), as it was loaded
	[[nodiscard]] core::ConcurrentAccess<std::string>& GetEnvironmentPath() noexcept;
	[[nodiscard]] core::ConcurrentAccess<std::vector<DominantLightInfo>>& GetEnvironmentLightInfo() noexcept;
	// the installed model's lights' sizes the ui set (LightData::size), by light: a directional light's angular radius in
	// degrees, a point or spot light's radius; NaN (or past the end) for the default, the sun's for a directional light
	// (environment::kSkySunRadius), 0 for a point or spot light (glTF's lights are punctual). cleared when a model is
	// installed
	[[nodiscard]] core::ConcurrentAccess<std::vector<float>>& GetLightSizes() noexcept;
	// the exposure the camera exposes with (see Settings::exposureMode: what is set, or what it chose for auto exposure's
	// stops, see camera::Choose): the first view's (each view has its own)
	[[nodiscard]] camera::Exposure GetViewExposure() const noexcept;
	// and the limits it exposes within (camera::ExposureLimits): the lens's aperture range (Settings::lens; the pinhole's
	// any), the rest the defaults
	[[nodiscard]] camera::ExposureLimits GetExposureLimits() const noexcept;
	// and the distance it focuses at (meters: Settings::focusDistance, or the first view's measure under autofocus)
	[[nodiscard]] float GetFocusDistance() const noexcept;
	// the bytes of device memory the renderer's transients take, and would take without aliasing
	[[nodiscard]] uint64_t GetTransientMemory() const noexcept;
	[[nodiscard]] uint64_t GetUnaliasedTransientMemory() const noexcept;
	// once ready (see Prepare)
	[[nodiscard]] const Renderer& GetRenderer() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> myImpl;
};

} // namespace gfx
