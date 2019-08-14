// wip: create generic gfx structures and functions. long term goal is to move all functions from vk-utils.h into gfx.h.
// wip: dynamic mesh layout, depending on input data structure.
// wip: create constructors/destructors for composite structs, and use shared_ptrs when referencing them. done: Texture, Model.
// wip: specialize on graphics backend
// wip: organize secondary command buffers into some sort of pool, and schedule them on a couple of worker threads
// wip: move stuff from headers into compilation units
// todo: extract descriptor sets
// todo: resource loading / manager
// todo: graph based GUI
// todo: frame graph
// todo: compute pipeline
// todo: clustered forward shading
// todo: shader graph

// done: separate IMGUI and volcano abstractions more clearly. avoid referencing IMGUI:s windowdata
// 		 members where possible
// done: instrumentation and timing information

#include "buffer.h"
#include "file.h"
#include "gfx.h"
#include "math.h"
#include "model.h"
#include "state.h"
#include "texture.h"
#include "utils.h"

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <cstdlib>
#if defined(__WINDOWS__)
#	include <execution>
#endif
#include <filesystem>
#include <future>
#include <iostream>
#include <locale>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

#include <nfd.h>

#include <Tracy.hpp>
#include <TracyVulkan.hpp>

#include <imgui.h>
#include <examples/imgui_impl_vulkan.h>


template <GraphicsBackend B>
struct WindowData
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t framebufferWidth = 0;
	uint32_t framebufferHeight = 0;

	SurfaceHandle<B> surface = 0;
	SurfaceFormat<B> surfaceFormat = {};
	PresentMode<B> presentMode;

	SwapchainContext<B> swapchain = {};

	std::shared_ptr<Texture<B>> zBuffer;
	ImageViewHandle<B> zBufferView = 0;
	
	std::vector<View> views;
	std::optional<size_t> activeView;

	// buffer for all views.
	std::shared_ptr<Buffer<B>> viewBuffer;
	//

	bool clearEnable = true;
    ClearValue<B> clearValue = {};
    
	uint32_t frameIndex = 0; // Current frame being rendered to (0 <= frameIndex < frames.count())
	uint32_t lastFrameIndex = 0; // Last frame being rendered to (0 <= frameIndex < frames.count())
	std::vector<FrameData<B>> frames;

	bool imguiEnable = true;
	bool createFrameResourcesFlag = false;
};

template <GraphicsBackend B>
struct GraphicsPipelineResourceView
{
	std::shared_ptr<Model<B>> model; // temp
	std::shared_ptr<Texture<B>> texture; // temp
	ImageViewHandle<B> textureView = 0;
	SamplerHandle<B> sampler = 0;

	std::shared_ptr<WindowData<B>> window; // temp - replace with generic render target structure
};

template <GraphicsBackend B>
struct PipelineConfiguration
{
	std::shared_ptr<GraphicsPipelineResourceView<B>> resources;
	std::shared_ptr<PipelineLayoutContext<B>> layout;

	RenderPassHandle<B> renderPass = 0; // should perhaps not be stored here...
	PipelineHandle<B> graphicsPipeline = 0; // ~ "PSO"

	std::vector<DescriptorSetHandle<B>> descriptorSets;
};

template <GraphicsBackend B>
class Application
{
public:

	Application(
		void* view, int width, int height, int framebufferWidth, int framebufferHeight,
		const char* resourcePath);

	~Application();

	void draw();

	void resizeWindow(const window_state& state);
	void resizeFramebuffer(int width, int height);

	void onMouse(const mouse_state& state);
	void onKeyboard(const keyboard_state& state);

private:

	// todo: encapsulate in Window/WindowData
	void createFrameResources(WindowData<B>& window);
	void cleanupFrameResources();
	void updateInput(WindowData<B>& window) const;
	void updateViewBuffer(WindowData<B>& window) const;
	void initIMGUI(WindowData<B>& window, float dpiScaleX, float dpiScaleY) const;
	void drawIMGUI(WindowData<B>& window);
	void checkFlipOrPresentResult(WindowData<B>& window, Result<B> result) const;
	void submitFrame(WindowData<B>& window);
	void presentFrame(WindowData<B>& window) const;

	// todo: move to gfx
	SurfaceHandle<B> createSurface(InstanceHandle<B> instance, void* view) const;
	AllocatorHandle<B> createAllocator(DeviceHandle<B> device, PhysicalDeviceHandle<B> physicalDevice) const;
	DescriptorPoolHandle<B> createDescriptorPool() const;
	RenderPassHandle<B> createRenderPass(DeviceHandle<B> device, const WindowData<B>& window) const;
	PipelineHandle<B> createGraphicsPipeline(
		DeviceHandle<B> device,
		PipelineCacheHandle<B> pipelineCache,
		const PipelineConfiguration<B>& pipelineConfig) const;
	//

	// todo: encapsulate in DeviceContext
	std::tuple<
		DeviceHandle<B>, PhysicalDeviceHandle<B>, PhysicalDeviceProperties<B>, SwapchainInfo<B>,
		SurfaceFormat<B>, PresentMode<B>, uint32_t, QueueHandle<B>, int>
	createDevice(InstanceHandle<B> instance, SurfaceHandle<B> surface) const;
	//

	// todo: encapsulate in SwapchainContext
	SwapchainContext<B> createSwapchainContext(
		DeviceHandle<B> device,
		PhysicalDeviceHandle<B> physicalDevice,
		AllocatorHandle<B> allocator,
		uint32_t frameCount,
		const WindowData<B>& window) const;
	//

	// todo: encapsulate in PipelineConfiguration
	PipelineConfiguration<B> createPipelineConfig(DeviceHandle<B> device, RenderPassHandle<B> renderPass,
		DescriptorPoolHandle<B> descriptorPool, PipelineCacheHandle<B> pipelineCache,
        std::shared_ptr<PipelineLayoutContext<B>> layoutContext, std::shared_ptr<GraphicsPipelineResourceView<B>> resources) const;
	void updateDescriptorSets(const WindowData<B>& window, const PipelineConfiguration<B>& pipelineConfig) const;
	//

	// todo: encapsulate in View/View
	void updateViewMatrix(View& view) const;
	void updateProjectionMatrix(View& view) const;
	//

	InstanceHandle<B> myInstance = 0;

	// todo: encapsulate in DeviceContext
	PhysicalDeviceHandle<B> myPhysicalDevice = 0;
	PhysicalDeviceProperties<B> myPhysicalDeviceProperties = {};
	DeviceHandle<B> myDevice = 0;
	// todo -> generic
	VkDebugReportCallbackEXT myDebugCallback = VK_NULL_HANDLE;
	TracyVkCtx myTracyContext;
	// end todo
	int myQueueFamilyIndex = -1;
	QueueHandle<B> myQueue = 0;
	AllocatorHandle<B> myAllocator = 0;
	DescriptorPoolHandle<B> myDescriptorPool = 0;
	std::vector<CommandPoolHandle<B>> myFrameCommandPools; // count = [threadCount]
	CommandPoolHandle<B> myTransferCommandPool = 0;
	PipelineCacheHandle<B> myPipelineCache = 0;
	//

	static constexpr uint32_t NX = 2;
	static constexpr uint32_t NY = 1;

	// todo: figure out best way of organizing these
	std::shared_ptr<GraphicsPipelineResourceView<B>> myDefaultResources;
	std::shared_ptr<PipelineLayoutContext<B>> myGraphicsPipelineLayout;
	std::shared_ptr<PipelineConfiguration<B>> myGraphicsPipelineConfig;
	//

	std::filesystem::path myResourcePath;

	uint32_t myCommandBufferThreadCount = 0;
	int myRequestedCommandBufferThreadCount = 0;

	std::map<int, bool> myKeysPressed;
	std::array<bool, 2> myMouseButtonsPressed;
	std::array<glm::vec2, 2> myMousePosition;
};

#include "application.inl"
