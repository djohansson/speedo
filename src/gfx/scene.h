#pragma once

#include <gfx/gpu.h>
#include <gfx/shaders/capi.h>

#include <core/concurrentaccess.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
		// the final image is scaled by 2^exposureStops before tonemapping (with auto exposure, on top of what it picks)
		std::atomic<float> exposureStops = 0.0F;
		std::atomic<bool> autoExposure = false;
		// the tonemapper (TONEMAPPER_*)
		std::atomic<uint32_t> tonemapper = TONEMAPPER_PBR_NEUTRAL;
		// the environment's intensity (radiance scale) and rotation about +y (degrees), and whether it is drawn as the
		// backdrop (see PushConstants::environmentIntensity)
		std::atomic<float> environmentIntensity = 1.0F;
		std::atomic<float> environmentRotationDegrees = 0.0F;
		std::atomic<bool> environmentBackdrop = true;
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

	// the installed model's file and its scenes (a gltf file's, see ModelDesc::scenes), for View > Scene. the draw
	// thread sets it, the ui thread reads it.
	struct SceneState
	{
		std::string filePath;
		std::vector<std::string> names;
		uint32_t current = 0;
	};

	// one of the installed environment's dominant lights, for the ui: its irradiance (lux, luminance) and whether the
	// model has the same light
	struct DominantLightInfo
	{
		float lux = 0.0F;
		bool merged = false;

		bool operator==(const DominantLightInfo&) const = default;
	};

	// what a frame is drawn into, and with (see Record)
	struct FrameTarget
	{
		uint16_t frameIndex = 0;
		// the swapchain (its current image), and its frame
		IRenderTarget* swapchain = nullptr;
		IRenderTarget* swapchainFrame = nullptr;
		Queue* graphicsQueue = nullptr;
		uint64_t graphicsTimeline = 0;
		// record the ui's texture uploads and draws (see Renderer's FrameInputs)
		std::function<void(CommandBufferHandle cmd)> prepareUi;
		std::function<void(CommandBufferHandle cmd)> drawUi;
		// runs a job on the task executor (pipelines created in the background)
		std::function<void(std::function<void()> job)> runInBackground;
	};

	// creates the default resources and the pipeline layouts, binds them, and sizes the renderer after the current
	// window's swapchain. views must outlive the scene.
	Scene(RHI& rhi, Views& views);
	~Scene();

	Scene(const Scene&) = delete;
	Scene& operator=(const Scene&) = delete;

	// what the pipeline's descriptor sets are allocated from (see RHIInitializationData::descriptorPoolSizes)
	[[nodiscard]] static std::vector<rhi::DescriptorPoolSize> DescriptorPoolSizes();

	// load, and have the draw thread install, unless the load is cancelled. call from a load task (see core::LoadQueue).
	// a model, or several side by side as one (see Model::Load), with its materials' textures. scene: a gltf file's
	// scene to load (one file only), else its default one
	void LoadModels(const std::vector<std::string>& filePaths, std::atomic_uint8_t& progress, std::optional<size_t> scene = std::nullopt);
	// an image, on the default material (material 0)
	void LoadImage(std::string_view filePath, std::atomic_uint8_t& progress);
	// an environment panorama (or without one, the procedural sky)
	void LoadEnvironment(std::optional<std::string> filePath, std::atomic_uint8_t& progress);

	// resizes what is sized after the current window's swapchain (its render targets), after the swapchain was
	// (re)created. call with the gpu idle, holding the draw lock.
	void Resize();

	// updates the frame's buffers for the frame about to be recorded, after its fence: the exposure histogram it read,
	// the animation and what it moves, and the lights. draw thread.
	void Update(QueueTimelineContextData& graphics, uint32_t frameIndex);

	// records the frame into cmd (see Renderer::Record). draw thread.
	void Record(CommandBufferHandle cmd, const FrameTarget& target);

	[[nodiscard]] Settings& GetSettings() noexcept;
	[[nodiscard]] core::ConcurrentAccess<AnimationState>& GetAnimation() noexcept;
	[[nodiscard]] core::ConcurrentAccess<SceneState>& GetScenes() noexcept;
	[[nodiscard]] core::ConcurrentAccess<std::string>& GetEnvironmentName() noexcept;
	[[nodiscard]] core::ConcurrentAccess<std::vector<DominantLightInfo>>& GetEnvironmentLightInfo() noexcept;
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
