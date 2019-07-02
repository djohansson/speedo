// wip: separate VK objects from generic ones.
// wip: dynamic mesh layout, depending on input data structure
// todo: create constructors/destructors for composite structs, and use shared_ptrs when referencing them.
// todo: specialize on graphics backend
// todo: organize secondary command buffers into some sort of pool, and schedule them on a couple of worker threads
// todo: move stuff from headers into compilation units
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

#include "volcano.h"
#include "aabb.h"
#include "file.h"
#include "gfx.h"
#include "glm.h"
#include "math.h"
#include "model.h"
#include "texture.h"
#include "utils.h"
#include "vertex.h"

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <algorithm>
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
#include <unordered_map>
#include <utility>
#include <vector>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

//#define TINYOBJLOADER_USE_EXPERIMENTAL
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
#	define TINYOBJ_LOADER_OPT_IMPLEMENTATION
#	include <experimental/tinyobj_loader_opt.h>
#else
#	define TINYOBJLOADER_IMPLEMENTATION
#	include <tiny_obj_loader.h>
#endif

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

#include <slang.h>

struct ViewData
{
	struct ViewportData
	{
		uint32_t width = 0;
		uint32_t height = 0;
	} viewport;

	glm::vec3 camPos = glm::vec3(0.0f, -2.0f, 0.0f);
	glm::vec3 camRot = glm::vec3(0.0f, 0.0f, 0.0);

	glm::mat4x3 view = glm::mat4x3(1.0f);
	glm::mat4 projection = glm::mat4(1.0f);
};

template <GraphicsBackend B>
struct FrameData
{
	uint32_t index = 0;
	
	FramebufferHandle<B> frameBuffer = 0;
	std::vector<CommandBufferHandle<B>> commandBuffers; // count = [threadCount]
	
	FenceHandle<B> fence = 0;
	SemaphoreHandle<B> renderCompleteSemaphore = 0;
	SemaphoreHandle<B> newImageAcquiredSemaphore = 0;

	std::chrono::high_resolution_clock::time_point graphicsFrameTimestamp;
	std::chrono::duration<double> graphicsDeltaTime;
};

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
	
	std::vector<ViewData> views;
	std::optional<size_t> activeView;

	bool clearEnable = true;
    ClearValue<B> clearValue = {};
    
	uint32_t frameIndex = 0; // Current frame being rendered to (0 <= frameIndex < frames.count())
	uint32_t lastFrameIndex = 0; // Last frame being rendered to (0 <= frameIndex < frames.count())
	std::vector<FrameData<B>> frames;

	BufferHandle<B> uniformBuffer = 0;
	AllocationHandle<B> uniformBufferMemory = 0;

	bool imguiEnable = true;
	bool createFrameResourcesFlag = false;
};

template <GraphicsBackend B>
struct GraphicsPipelineResourceContext // temp
{
	std::shared_ptr<Model<B>> model;
	std::shared_ptr<Texture<B>> texture;
	SamplerHandle<B> sampler = 0;

	std::shared_ptr<WindowData<B>> window; // temp - replace with generic render target structure
};

template <GraphicsBackend B>
struct PipelineConfiguration
{
	std::shared_ptr<GraphicsPipelineResourceContext<B>> resources;
	std::shared_ptr<PipelineLayoutContext<B>> layout;
	RenderPassHandle<B> renderPass = 0;
};

using EntryPoint = std::pair<std::string, uint32_t>;
using ShaderBinary = std::vector<std::byte>;
using ShaderEntry = std::pair<ShaderBinary, EntryPoint>;

template <GraphicsBackend B>
struct SerializableDescriptorSetLayoutBinding : public DescriptorSetLayoutBinding<B>
{
	using BaseType = DescriptorSetLayoutBinding<B>;

	template <class Archive, GraphicsBackend B = B>
	typename std::enable_if_t<B == GraphicsBackend::Vulkan, void> serialize(Archive& ar)
	{
		static_assert(sizeof(*this) == sizeof(BaseType));

		ar(BaseType::binding);
		ar(BaseType::descriptorType);
		ar(BaseType::descriptorCount);
		ar(BaseType::stageFlags);
		// ar(pImmutableSamplers); // todo
	}
};

// this is a temporary object only used during loading.
template <GraphicsBackend B>
struct SerializableShaderReflectionModule
{
	using ShaderEntryVector = std::vector<ShaderEntry>;
	using BindingsMap =
		std::map<uint32_t, std::vector<SerializableDescriptorSetLayoutBinding<B>>>; // set, bindings

	template <class Archive>
	void serialize(Archive& ar)
	{
		ar(shaders);
		ar(bindings);
	}

	ShaderEntryVector shaders;
	BindingsMap bindings;
};

#pragma pack(push, 1)
template <GraphicsBackend B>
struct PipelineCacheHeader
{
};
#pragma pack(pop)

#pragma pack(push, 1)
template <>
struct PipelineCacheHeader<GraphicsBackend::Vulkan>
{
	uint32_t headerLength = 0;
	uint32_t cacheHeaderVersion = 0;
	uint32_t vendorID = 0;
	uint32_t deviceID = 0;
	uint8_t pipelineCacheUUID[VK_UUID_SIZE] = {};
};
#pragma pack(pop)

template <GraphicsBackend B>
class Application
{
public:
	Application(
		void* view, int width, int height, int framebufferWidth, int framebufferHeight,
		const char* resourcePath)
		: myResourcePath(resourcePath)
		, myCommandBufferThreadCount(2)
		, myRequestedCommandBufferThreadCount(2)
	{
		ZoneScoped;

		assert(std::filesystem::is_directory(myResourcePath));

		myInstance = createInstance();
		myDebugCallback = createDebugCallback(myInstance);

		myDefaultResources = std::make_shared<GraphicsPipelineResourceContext<B>>();
		myDefaultResources->window = std::make_shared<WindowData<B>>();
		auto& window = *myDefaultResources->window;

		window.width = width;
		window.height = height;
		window.framebufferWidth = framebufferWidth;
		window.framebufferHeight = framebufferHeight;
		window.surface = createSurface(myInstance, view);

		uint32_t frameCount = 0;

		std::tie(
			myDevice, myPhysicalDevice, myPhysicalDeviceProperties, window.swapchain.info, window.surfaceFormat,
			window.presentMode, frameCount, myQueue, myQueueFamilyIndex) =
			createDevice(myInstance, window.surface);

		myAllocator = createAllocator(myDevice, myPhysicalDevice);

		myTransferCommandPool = createCommandPool(
			myDevice,
			VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
			myQueueFamilyIndex);

		myDefaultResources->sampler = createTextureSampler(myDevice);
		myDescriptorPool = createDescriptorPool();

		auto slangShaders = loadSlangShaders(std::filesystem::absolute(myResourcePath / "shaders" / "shaders.slang"));

		myGraphicsPipelineLayout = std::make_shared<PipelineLayoutContext<B>>();
		*myGraphicsPipelineLayout = createPipelineLayoutContext(myDevice, *slangShaders);
		
		myDefaultResources->model = loadModel(std::filesystem::absolute(myResourcePath / "models" / "gallery.obj"));
		myDefaultResources->texture = loadTexture(std::filesystem::absolute(myResourcePath / "images" / "gallery.jpg"));

		createBuffer(
			myAllocator, frameCount * (NX * NY) * sizeof(UniformBufferObject), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, window.uniformBuffer,
			window.uniformBufferMemory, "uniformBuffer");

		myPipelineCache = loadPipelineCache(
			myDevice,
			myPhysicalDeviceProperties,
			std::filesystem::absolute(myResourcePath / ".cache" / "pipeline.cache"));

		createSwapchain(myDevice, myPhysicalDevice, myAllocator, frameCount, *myDefaultResources->window);

		std::vector<Model<B>*> models;
		models.push_back(myDefaultResources->model.get());
		createFrameResources(*myDefaultResources->window, models);

		float dpiScaleX = static_cast<float>(framebufferWidth) / width;
		float dpiScaleY = static_cast<float>(framebufferHeight) / height;

		initIMGUI(window, dpiScaleX, dpiScaleY);
	}

	~Application()
	{
		ZoneScoped;

		{
			ZoneScopedN("deviceWaitIdle");

			CHECK_VK(vkDeviceWaitIdle(myDevice));
		}

		std::filesystem::path cacheFilePath = std::filesystem::absolute(myResourcePath / ".cache");

		if (!std::filesystem::exists(cacheFilePath))
			std::filesystem::create_directory(cacheFilePath);

		savePipelineCache(myDevice, myPipelineCache, myPhysicalDeviceProperties, cacheFilePath / "pipeline.cache");

		cleanup();
	}

	void draw()
	{
		FrameMark;
		ZoneScoped;

		auto& window = *myDefaultResources->window;

		// re-create frame resources if needed
		if (window.createFrameResourcesFlag)
		{
			ZoneScopedN("recreateFrameResources");

			{
				ZoneScopedN("queueWaitIdle");

				CHECK_VK(vkQueueWaitIdle(myQueue));
			}

			cleanupFrameResources();

			myCommandBufferThreadCount = std::min(static_cast<uint32_t>(myRequestedCommandBufferThreadCount), NX * NY);

			std::vector<Model<B>*> models;
			models.push_back(myDefaultResources->model.get());
			createFrameResources(window, models);
		}

		updateInput(window);
		submitFrame(window);
		presentFrame(window);
	}

	void resizeWindow(const window_state& state)
	{
		auto& window = *myDefaultResources->window;

		if (state.fullscreen_enabled)
		{
			window.width = state.fullscreen_width;
			window.height = state.fullscreen_height;
		}
		else
		{
			window.width = state.width;
			window.height = state.height;
		}
	}

	void resizeFramebuffer(int width, int height)
	{
		ZoneScoped;

		auto& window = *myDefaultResources->window;

		{
			ZoneScopedN("queueWaitIdle");

			CHECK_VK(vkQueueWaitIdle(myQueue));
		}

		cleanupFrameResources();

		// hack to shut up validation layer createPipelineLayoutContext.
		// see https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/624
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
			myPhysicalDevice, window.surface, &window.swapchain.info.capabilities);

		// window.width = width;
		// window.height = height;
		window.framebufferWidth = width;
		window.framebufferHeight = height;

		createSwapchain(myDevice, myPhysicalDevice, myAllocator, window.swapchain.colorImages.size(), window);

		std::vector<Model<B>*> models;
		models.push_back(myDefaultResources->model.get());
		createFrameResources(*myDefaultResources->window, models);
	}

	void onMouse(const mouse_state& state)
	{
		ZoneScoped;

		auto& window = *myDefaultResources->window;

		bool leftPressed = state.button == GLFW_MOUSE_BUTTON_LEFT && state.action == GLFW_PRESS;
		bool rightPressed = state.button == GLFW_MOUSE_BUTTON_RIGHT && state.action == GLFW_PRESS;
		
		auto screenPos = glm::vec2(state.xpos, state.ypos);
		// auto screenPos = ImGui::GetCursorScreenPos();

		if (state.inside_window && !myMouseButtonsPressed[0])
		{
			// todo: generic view index calculation
			size_t viewIdx = screenPos.x / (window.width / NX);
			size_t viewIdy = screenPos.y / (window.height / NY);
			window.activeView = std::min((viewIdy * NX) + viewIdx, window.views.size() - 1);

			// std::cout << *window.activeView << ":[" << screenPos.x << ", " << screenPos.y << "]"
			// 		  << std::endl;
		}
		else if (!leftPressed)
		{
			window.activeView.reset();

			// std::cout << "window.activeView.reset()" << std::endl;
		}

		myMousePosition[0] =
			glm::vec2{static_cast<float>(screenPos.x), static_cast<float>(screenPos.y)};
		myMousePosition[1] =
			leftPressed && !myMouseButtonsPressed[0] ? myMousePosition[0] : myMousePosition[1];

		myMouseButtonsPressed[0] = leftPressed;
		myMouseButtonsPressed[1] = rightPressed;
	}

	void onKeyboard(const keyboard_state& state)
	{
		ZoneScoped;

		if (state.action == GLFW_PRESS)
			myKeysPressed[state.key] = true;
		else if (state.action == GLFW_RELEASE)
			myKeysPressed[state.key] = false;
	}

private:

	void updateInput(WindowData<B>& window) const
	{
		ZoneScoped;

		auto& frame = window.frames[window.lastFrameIndex];
		float dt = frame.graphicsDeltaTime.count();

		// update input dependent state
		{
			auto& io = ImGui::GetIO();

			static bool escBufferState = false;
			bool escState = io.KeysDown[io.KeyMap[ImGuiKey_Escape]];
			if (escState && !escBufferState)
				window.imguiEnable = !window.imguiEnable;
			escBufferState = escState;
		}

		if (myCommandBufferThreadCount != myRequestedCommandBufferThreadCount)
			window.createFrameResourcesFlag = true;

		if (window.activeView)
		{
			// std::cout << "window.activeView read/consume" << std::endl;

			float dx = 0;
			float dz = 0;

			for (const auto& [key, pressed] : myKeysPressed)
			{
				if (pressed)
				{
					switch (key)
					{
					case GLFW_KEY_W:
						dz = 1;
						break;
					case GLFW_KEY_S:
						dz = -1;
						break;
					case GLFW_KEY_A:
						dx = 1;
						break;
					case GLFW_KEY_D:
						dx = -1;
						break;
					default:
						break;
					}
				}
			}

			auto& view = window.views[*window.activeView];

			bool doUpdateViewMatrix = false;

			if (dx != 0 || dz != 0)
			{
				auto forward = glm::vec3(view.view[0][2], view.view[1][2], view.view[2][2]);
				auto strafe = glm::vec3(view.view[0][0], view.view[1][0], view.view[2][0]);

				constexpr auto moveSpeed = 2.0f;

				view.camPos += dt * (dz * forward + dx * strafe) * moveSpeed;

				// std::cout << *window.activeView << ":pos:[" << view.camPos.x << ", " <<
				// view.camPos.y << ", " << view.camPos.z << "]" << std::endl;

				doUpdateViewMatrix = true;
			}

			if (myMouseButtonsPressed[0])
			{
				constexpr auto rotSpeed = 10.0f;

				auto dM = myMousePosition[0] - myMousePosition[1];

				view.camRot +=
					dt * glm::vec3(dM.y / view.viewport.height, dM.x / view.viewport.width, 0.0f) *
					rotSpeed;

				// std::cout << *window.activeView << ":rot:[" << view.camRot.x << ", " <<
				// view.camRot.y << ", " << view.camRot.z << "]" << std::endl;

				doUpdateViewMatrix = true;
			}

			if (doUpdateViewMatrix)
			{
				updateViewMatrix(window.views[*window.activeView]);
			}
		}
	}

	void drawIMGUI(WindowData<B>& window)
	{
		ZoneScoped;

		using namespace ImGui;

		ImGui_ImplVulkan_NewFrame();

		NewFrame();
		ShowDemoWindow();

		{
			Begin("Render Options");
			DragInt(
				"Command Buffer Threads", &myRequestedCommandBufferThreadCount, 0.1f, 2, 32);
			ColorEdit3(
				"Clear Color", &window.clearValue.color.float32[0]);
			End();
		}

		{
			Begin("GUI Options");
			// static int styleIndex = 0;
			ShowStyleSelector("Styles" /*, &styleIndex*/);
			ShowFontSelector("Fonts");
			if (Button("Show User Guide"))
			{
				SetNextWindowPosCenter(0);
				OpenPopup("UserGuide");
			}
			if (BeginPopup(
					"UserGuide", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
			{
				ShowUserGuide();
				EndPopup();
			}
			End();
		}

		{
			Begin("File");

			if (Button("Open OBJ file"))
            {
                nfdchar_t* pathStr;
                auto res = NFD_OpenDialog("obj", myResourcePath.u8string().c_str(), &pathStr);
                if (res == NFD_OKAY)
                {
                    myDefaultResources->model = loadModel(std::filesystem::absolute(pathStr));

					updateDescriptorSets(window);
                }
            }

			if (Button("Open JPG file"))
            {
                nfdchar_t* pathStr;
                auto res = NFD_OpenDialog("jpg", myResourcePath.u8string().c_str(), &pathStr);
                if (res == NFD_OKAY)
                {
                    myDefaultResources->texture = loadTexture(std::filesystem::absolute(pathStr));

					updateDescriptorSets(window);
                }
            }

			End();
		}

		{
			ShowMetricsWindow();
		}

		Render();
	}

	std::shared_ptr<Model<B>> loadModel(const std::filesystem::path& modelFile) const
	{
		ZoneScoped;

		VertexAllocator vertices;
		std::vector<uint32_t> indices;
		std::vector<SerializableVertexInputAttributeDescription<B>> attributeDescriptions;
		AABB3f aabb;

		auto loadPBin = [&vertices, &indices, &attributeDescriptions, &aabb](std::istream& stream) {
			cereal::PortableBinaryInputArchive pbin(stream);
			pbin(vertices, indices, attributeDescriptions, aabb);
		};

		auto savePBin = [&vertices, &indices, &attributeDescriptions, &aabb](std::ostream& stream) {
			cereal::PortableBinaryOutputArchive pbin(stream);
			pbin(vertices, indices, attributeDescriptions, aabb);
		};

		auto loadOBJ = [&vertices, &indices, &attributeDescriptions, &aabb](std::istream& stream) {
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
			using namespace tinyobj_opt;
#else
			using namespace tinyobj;
#endif
			attrib_t attrib;
			std::vector<shape_t> shapes;
			std::vector<material_t> materials;
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
			std::streambuf* raw_buffer = stream.rdbuf();
			std::streamsize bufferSize = stream.gcount();
			std::unique_ptr<char[]> buffer = std::make_unique<char[]>(bufferSize);
			raw_buffer->sgetn(buffer.get(), bufferSize);
			LoadOption option;
			if (!parseObj(&attrib, &shapes, &materials, buffer.get(), bufferSize, option))
				throw std::runtime_error("Failed to load model.");
#else
			std::string warn, err;
			if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, &stream))
				throw std::runtime_error(err);
#endif

			uint32_t indexCount = 0;
			for (const auto& shape : shapes)
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
				for (uint32_t faceOffset = shape.face_offset;
					 faceOffset < (shape.face_offset + shape.length);
					 faceOffset++)
					indexCount += attrib.face_num_verts[faceOffset];
#else
				indexCount += shape.mesh.indices.size();
#endif

			attributeDescriptions.emplace_back(
				SerializableVertexInputAttributeDescription<B>{0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 0u});
			attributeDescriptions.emplace_back(
				SerializableVertexInputAttributeDescription<B>{1u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 12u});
			attributeDescriptions.emplace_back(
				SerializableVertexInputAttributeDescription<B>{2u, 0u, VK_FORMAT_R32G32_SFLOAT, 24u});

			std::unordered_map<uint64_t, uint32_t> uniqueVertices;

			vertices.setStride(32);

			ScopedVertexAllocation vertexScope(vertices);
			vertices.reserve(indexCount / 3); // guesstimate
			indices.reserve(indexCount);

			size_t posOffset = attributeDescriptions[0].offset;
			size_t colorOffset = attributeDescriptions[1].offset;
			size_t texCoordOffset = attributeDescriptions[2].offset;

			for (const auto& shape : shapes)
			{
#ifdef TINYOBJLOADER_USE_EXPERIMENTAL
				for (uint32_t faceOffset = shape.face_offset;
					 faceOffset < (shape.face_offset + shape.length);
					 faceOffset++)
				{
					const index_t& index = attrib.indices[faceOffset];
#else
				for (const auto& index : shape.mesh.indices)
				{
#endif
					auto& vertex = *vertexScope.createVertices();

					glm::vec3* pos = vertex.dataAs<glm::vec3>(posOffset);
					*pos = {attrib.vertices[3 * index.vertex_index + 0],
							attrib.vertices[3 * index.vertex_index + 1],
							attrib.vertices[3 * index.vertex_index + 2]};

					glm::vec3* color = vertex.dataAs<glm::vec3>(colorOffset);
					*color = {1.0f, 1.0f, 1.0f};

					glm::vec2* texCoord = vertex.dataAs<glm::vec2>(texCoordOffset);
					*texCoord = {attrib.texcoords[2 * index.texcoord_index + 0],
								 1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};

					uint64_t vertexHash = vertex.hash();

					if (uniqueVertices.count(vertexHash) == 0)
					{
						uniqueVertices[vertexHash] = static_cast<uint32_t>(vertices.size() - 1);
						aabb.merge(*pos);
					}
					else
					{
						vertexScope.freeVertices(&vertex);
					}

					indices.push_back(uniqueVertices[vertexHash]);
				}
			}
		};

		loadCachedSourceFile(
			modelFile, modelFile, "tinyobjloader", "1.4.0", loadOBJ, loadPBin, savePBin);

		if (vertices.empty() || indices.empty())
			throw std::runtime_error("Failed to load model.");

		return std::make_shared<Model<B>>(
			myDevice, myTransferCommandPool, myQueue, myAllocator,
			vertices.data(), vertices.sizeBytes(),
			(const std::byte*)indices.data(), indices.size(), sizeof(uint32_t), attributeDescriptions, modelFile.u8string().c_str());
	}

	std::shared_ptr<Texture<B>> loadTexture(const std::filesystem::path& imageFile) const
	{
		ZoneScoped;

		int nx, ny, nChannels;
		size_t dxtImageSize;
		std::unique_ptr<std::byte[]> dxtImageData;

		auto loadPBin = [&nx, &ny, &nChannels, &dxtImageData, &dxtImageSize](std::istream& stream) {
			cereal::PortableBinaryInputArchive pbin(stream);
			pbin(nx, ny, nChannels, dxtImageSize);
			dxtImageData = std::make_unique<std::byte[]>(dxtImageSize);
			pbin(cereal::binary_data(dxtImageData.get(), dxtImageSize));
		};

		auto savePBin = [&nx, &ny, &nChannels, &dxtImageData, &dxtImageSize](std::ostream& stream) {
			cereal::PortableBinaryOutputArchive pbin(stream);
			pbin(nx, ny, nChannels, dxtImageSize);
			pbin(cereal::binary_data(dxtImageData.get(), dxtImageSize));
		};

		auto loadImage = [&nx, &ny, &nChannels, &dxtImageData,
						  &dxtImageSize](std::istream& stream) {
			stbi_io_callbacks callbacks;
			callbacks.read = &stbi_istream_callbacks::read;
			callbacks.skip = &stbi_istream_callbacks::skip;
			callbacks.eof = &stbi_istream_callbacks::eof;
			stbi_uc* imageData =
				stbi_load_from_callbacks(&callbacks, &stream, &nx, &ny, &nChannels, STBI_rgb_alpha);

			bool hasAlpha = nChannels == 4;
			uint32_t compressedBlockSize = hasAlpha ? 16 : 8;
			dxtImageSize = hasAlpha ? nx * ny : nx * ny / 2;
			dxtImageData = std::make_unique<std::byte[]>(dxtImageSize);

			auto extractBlock = [](const stbi_uc* src, uint32_t width, uint32_t stride,
								   stbi_uc* dst) {
				for (uint32_t rowIt = 0; rowIt < 4; rowIt++)
				{
					std::copy(src, src + stride * 4, &dst[rowIt * 16]);
					src += width * stride;
				}
			};

			stbi_uc block[64] = {0};
			const stbi_uc* src = imageData;
			stbi_uc* dst = reinterpret_cast<stbi_uc*>(dxtImageData.get());
			for (uint32_t rowIt = 0; rowIt < ny; rowIt += 4)
			{
				for (uint32_t colIt = 0; colIt < nx; colIt += 4)
				{
					uint32_t offset = (rowIt * nx + colIt) * 4;
					extractBlock(src + offset, nx, 4, block);
					stb_compress_dxt_block(dst, block, hasAlpha, STB_DXT_HIGHQUAL);
					dst += compressedBlockSize;
				}
			}

			stbi_image_free(imageData);
		};

		loadCachedSourceFile(
			imageFile, imageFile, "stb_image|stb_dxt", "2.20|1.08b", loadImage, loadPBin, savePBin);

		if (dxtImageData == nullptr)
			throw std::runtime_error("Failed to load image.");

		return std::make_shared<Texture<B>>(
			myDevice, myTransferCommandPool, myQueue, myAllocator,
			dxtImageData.get(), nx, ny,
			nChannels == 3 ? VK_FORMAT_BC1_RGB_UNORM_BLOCK
						   : VK_FORMAT_BC5_UNORM_BLOCK, // todo: write utility function for this
			VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, imageFile.u8string().c_str());
	}

	std::shared_ptr<SerializableShaderReflectionModule<B>> loadSlangShaders(const std::filesystem::path& slangFile) const
	{
		ZoneScoped;

		auto slangModule = std::make_shared<SerializableShaderReflectionModule<B>>();

		auto loadPBin = [&slangModule](std::istream& stream) {
			cereal::PortableBinaryInputArchive pbin(stream);
			pbin(*slangModule);
		};

		auto savePBin = [&slangModule](std::ostream& stream) {
			cereal::PortableBinaryOutputArchive pbin(stream);
			pbin(*slangModule);
		};

		auto loadSlang = [&slangModule, &slangFile](std::istream& stream) {
			SlangSession* slangSession = spCreateSession(NULL);
			SlangCompileRequest* slangRequest = spCreateCompileRequest(slangSession);

			spSetDumpIntermediates(slangRequest, true);

			int targetIndex = spAddCodeGenTarget(slangRequest, SLANG_SPIRV);
			(void)targetIndex;
			int translationUnitIndex =
				spAddTranslationUnit(slangRequest, SLANG_SOURCE_LANGUAGE_SLANG, nullptr);

			std::string shaderString(
				(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

			spAddTranslationUnitSourceStringSpan(
				slangRequest, translationUnitIndex, slangFile.u8string().c_str(),
				shaderString.c_str(), shaderString.c_str() + shaderString.size());

			// temp
			const char* epStrings[] = {"vertexMain", "fragmentMain"};
			const SlangStage epStages[] = {SLANG_STAGE_VERTEX, SLANG_STAGE_FRAGMENT};
			// end temp

			static_assert(sizeof_array(epStrings) == sizeof_array(epStages));

			std::vector<EntryPoint> entryPoints;
			for (int i = 0; i < sizeof_array(epStrings); i++)
			{
				int index =
					spAddEntryPoint(slangRequest, translationUnitIndex, epStrings[i], epStages[i]);

				if (index != entryPoints.size())
					throw std::runtime_error("Failed to add entry point.");

				entryPoints.push_back(std::make_pair(epStrings[i], epStages[i]));
			}

			const SlangResult compileRes = spCompile(slangRequest);

			if (auto diagnostics = spGetDiagnosticOutput(slangRequest))
				std::cout << diagnostics << std::endl;

			if (SLANG_FAILED(compileRes))
			{
				spDestroyCompileRequest(slangRequest);
				spDestroySession(slangSession);

				throw std::runtime_error("Failed to compile slang shader module.");
			}

			int depCount = spGetDependencyFileCount(slangRequest);
			for (int dep = 0; dep < depCount; dep++)
			{
				char const* depPath = spGetDependencyFilePath(slangRequest, dep);
				// ... todo: add dependencies for hot reload
				std::cout << depPath << std::endl;
			}

			for (const auto& ep : entryPoints)
			{
				ISlangBlob* blob = nullptr;
				if (SLANG_FAILED(
						spGetEntryPointCodeBlob(slangRequest, &ep - &entryPoints[0], 0, &blob)))
				{
					spDestroyCompileRequest(slangRequest);
					spDestroySession(slangSession);

					throw std::runtime_error("Failed to get slang blob.");
				}

				slangModule->shaders.emplace_back(std::make_pair(blob->getBufferSize(), ep));
				std::copy(
					static_cast<const std::byte*>(blob->getBufferPointer()),
					static_cast<const std::byte*>(blob->getBufferPointer()) + blob->getBufferSize(),
					slangModule->shaders.back().first.data());
				blob->release();
			}

			auto& bindings = slangModule->bindings;

			auto createLayoutBinding = [&bindings](slang::VariableLayoutReflection* parameter) {
				slang::TypeLayoutReflection* typeLayout = parameter->getTypeLayout();

				std::cout << "name: " << parameter->getName()
						  << ", index: " << parameter->getBindingIndex()
						  << ", space: " << parameter->getBindingSpace()
						  << ", stage: " << parameter->getStage()
						  << ", kind: " << (int)typeLayout->getKind()
						  << ", typeName: " << typeLayout->getName();

				unsigned categoryCount = parameter->getCategoryCount();
				for (unsigned cc = 0; cc < categoryCount; cc++)
				{
					slang::ParameterCategory category = parameter->getCategoryByIndex(cc);

					size_t offsetForCategory = parameter->getOffset(category);
					size_t spaceForCategory = parameter->getBindingSpace(category);

					std::cout << ", category: " << category
							  << ", offsetForCategory: " << offsetForCategory
							  << ", spaceForCategory: " << spaceForCategory;

					if (category == slang::ParameterCategory::DescriptorTableSlot)
					{
						using MapType = std::remove_reference_t<decltype(bindings)>;
						using VectorType = typename MapType::mapped_type;
						using BindingType = typename VectorType::value_type;

						BindingType binding;
						binding.binding = parameter->getBindingIndex();
						binding.descriptorCount =
							typeLayout->isArray() ? typeLayout->getElementCount() : 1;
						binding.stageFlags =
							VK_SHADER_STAGE_ALL; // todo: have not find a good way to get a good
												 // value for this yet
						binding.pImmutableSamplers = nullptr; // todo;

						switch (parameter->getStage())
						{
						case SLANG_STAGE_VERTEX:
							binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
							break;
						case SLANG_STAGE_FRAGMENT:
							binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
							break;
						case SLANG_STAGE_HULL:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_DOMAIN:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_GEOMETRY:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_COMPUTE:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_RAY_GENERATION:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_INTERSECTION:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_ANY_HIT:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_CLOSEST_HIT:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_MISS:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_CALLABLE:
							assert(false); // please implement me!
							break;
						case SLANG_STAGE_NONE:
							// assert(false); // this seems to be returned for my constant buffer.
							// investigate why. perhaps that it is reused by multiple shaders? skip
							// assert for now.
							break;
						default:
							assert(false); // please implement me!
							break;
						}

						switch (typeLayout->getKind())
						{
						case slang::TypeReflection::Kind::ConstantBuffer:
							binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
							break;
						case slang::TypeReflection::Kind::Resource:
							binding.descriptorType =
								VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; // "resource" might be more
																  // generic tho...
							break;
						case slang::TypeReflection::Kind::SamplerState:
							binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
							break;
						case slang::TypeReflection::Kind::None:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::Struct:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::Array:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::Matrix:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::Vector:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::Scalar:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::TextureBuffer:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::ShaderStorageBuffer:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::ParameterBlock:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::GenericTypeParameter:
							assert(false); // please implement me!
							break;
						case slang::TypeReflection::Kind::Interface:
							assert(false); // please implement me!
							break;
						default:
							assert(false); // please implement me!
							break;
						}

						bindings[parameter->getBindingSpace()].push_back(binding);
					}
				}

				std::cout << std::endl;

				unsigned fieldCount = typeLayout->getFieldCount();
				for (unsigned ff = 0; ff < fieldCount; ff++)
				{
					slang::VariableLayoutReflection* field = typeLayout->getFieldByIndex(ff);

					std::cout << "  name: " << field->getName()
							  << ", index: " << field->getBindingIndex()
							  << ", space: " << field->getBindingSpace(field->getCategory())
							  << ", offset: " << field->getOffset(field->getCategory())
							  << ", kind: " << (int)field->getType()->getKind()
							  << ", typeName: " << field->getType()->getName() << std::endl;
				}
			};

			slang::ShaderReflection* shaderReflection = slang::ShaderReflection::get(slangRequest);

			for (unsigned pp = 0; pp < shaderReflection->getParameterCount(); pp++)
				createLayoutBinding(shaderReflection->getParameterByIndex(pp));

			for (uint32_t epIndex = 0; epIndex < shaderReflection->getEntryPointCount(); epIndex++)
			{
				slang::EntryPointReflection* epReflection =
					shaderReflection->getEntryPointByIndex(epIndex);

				for (unsigned pp = 0; pp < epReflection->getParameterCount(); pp++)
					createLayoutBinding(epReflection->getParameterByIndex(pp));
			}

			spDestroyCompileRequest(slangRequest);
			spDestroySession(slangSession);
		};

		loadCachedSourceFile(
			slangFile, slangFile, "slang", "1.0.0-dev", loadSlang, loadPBin, savePBin);

		if (slangModule->shaders.empty())
			throw std::runtime_error("Failed to load shaders.");

		return slangModule;
	}

	PipelineCacheHandle<B> loadPipelineCache(
		DeviceHandle<B> device,
		PhysicalDeviceProperties<B> physicalDeviceProperties,
		const std::filesystem::path& cacheFilePath) const
	{
		ZoneScopedN("loadPipelineCache");

		std::vector<std::byte> cacheData;

		auto loadCache = [&physicalDeviceProperties, &cacheData](std::istream& stream) {
			cereal::BinaryInputArchive bin(stream);
			bin(cacheData);

			const PipelineCacheHeader<GraphicsBackend::Vulkan>* header =
				reinterpret_cast<const PipelineCacheHeader<GraphicsBackend::Vulkan>*>(cacheData.data());

			std::cout << "headerLength: " << header->headerLength << "\n";
			std::cout << "cacheHeaderVersion: " << header->cacheHeaderVersion << "\n";
			std::cout << "vendorID: " << header->vendorID << "\n";
			std::cout << "deviceID: " << header->deviceID << "\n";
			std::cout << "pipelineCacheUUID: ";
			for (uint32_t i = 0; i < VK_UUID_SIZE; i++)
				std::cout << header->pipelineCacheUUID[i];
			std::cout << std::endl;

			if (header->headerLength <= 0 ||
				header->cacheHeaderVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
				header->vendorID != physicalDeviceProperties.vendorID ||
				header->deviceID != physicalDeviceProperties.deviceID ||
				memcmp(header->pipelineCacheUUID, physicalDeviceProperties.pipelineCacheUUID, sizeof(header->pipelineCacheUUID)) != 0)
			{
				std::cout << "Invalid pipeline cache, creating new." << std::endl;
				cacheData.clear();
				return;
			}
		};

		FileInfo sourceFileInfo;
		if (getFileInfo(cacheFilePath, sourceFileInfo, false) != FileState::Missing)
			loadBinaryFile(cacheFilePath, sourceFileInfo, loadCache, false);

		auto createPipelineCache = [](DeviceHandle<B> device, const std::vector<std::byte>& cacheData)
		{
			ZoneScopedN("createPipelineCache");

			PipelineCacheHandle<B> cache;

			VkPipelineCacheCreateInfo createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
			createInfo.flags = 0;
			createInfo.initialDataSize = cacheData.size();
			createInfo.pInitialData = cacheData.size() ? cacheData.data() : nullptr;
		
			CHECK_VK(vkCreatePipelineCache(device, &createInfo, nullptr, &cache));
		
			return cache;
		};

		return createPipelineCache(device, cacheData);
	};

	void savePipelineCache(
		DeviceHandle<B> device,
		PipelineCacheHandle<B> pipelineCache,
		PhysicalDeviceProperties<B> physicalDeviceProperties,
		const std::filesystem::path& cacheFilePath) const
	{
		ZoneScoped;

		auto savePipelineCacheData = [&device, &pipelineCache, &physicalDeviceProperties](std::ostream& stream) {
			ZoneScopedN("savePipelineCacheData");
			
			size_t cacheDataSize = 0;
			CHECK_VK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, nullptr));
			if (cacheDataSize)
			{
				std::vector<std::byte> cacheData(cacheDataSize);
				CHECK_VK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, cacheData.data()));

				const PipelineCacheHeader<GraphicsBackend::Vulkan>* header =
					reinterpret_cast<const PipelineCacheHeader<GraphicsBackend::Vulkan>*>(cacheData.data());

				std::cout << "headerLength: " << header->headerLength << "\n";
				std::cout << "cacheHeaderVersion: " << header->cacheHeaderVersion << "\n";
				std::cout << "vendorID: " << header->vendorID << "\n";
				std::cout << "deviceID: " << header->deviceID << "\n";
				std::cout << "pipelineCacheUUID: ";
				for (uint32_t i = 0; i < VK_UUID_SIZE; i++)
					std::cout << header->pipelineCacheUUID[i];
				std::cout << std::endl;

				if (header->headerLength <= 0 ||
					header->cacheHeaderVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
					header->vendorID != physicalDeviceProperties.vendorID ||
					header->deviceID != physicalDeviceProperties.deviceID ||
					memcmp(header->pipelineCacheUUID, physicalDeviceProperties.pipelineCacheUUID, sizeof(header->pipelineCacheUUID)) != 0)
				{
					std::cout << "Invalid pipeline cache, something is seriously wrong. Exiting." << std::endl;
					return;
				}
				
				cereal::BinaryOutputArchive bin(stream);
				bin(cacheData);
			}
			else
				assert(false && "Failed to get pipeline cache.");
		};

		FileInfo cacheFileInfo;
		saveBinaryFile(cacheFilePath, cacheFileInfo, savePipelineCacheData, false);
	}

	PipelineLayoutContext<B> createPipelineLayoutContext(
		DeviceHandle<B> device,
		const SerializableShaderReflectionModule<B>& slangModule) const
	{
		ZoneScoped;

		PipelineLayoutContext<B> pipelineLayout;

		auto shaderDeleter = [device](ShaderModuleHandle<B>* module, size_t size) {
			for (size_t i = 0; i < size; i++)
				vkDestroyShaderModule(device, *(module + i), nullptr);
		};
		pipelineLayout.shaders =
			std::unique_ptr<ShaderModuleHandle<B>[], ArrayDeleter<ShaderModuleHandle<B>>>(
				new ShaderModuleHandle<B>[slangModule.shaders.size()],
				{shaderDeleter, slangModule.shaders.size()});

		auto descriptorSetLayoutDeleter =
			[device](DescriptorSetLayoutHandle<B>* layout, size_t size) {
				for (size_t i = 0; i < size; i++)
					vkDestroyDescriptorSetLayout(device, *(layout + i), nullptr);
			};
		pipelineLayout.descriptorSetLayouts = std::unique_ptr<
			DescriptorSetLayoutHandle<B>[], ArrayDeleter<DescriptorSetLayoutHandle<B>>>(
			new DescriptorSetLayoutHandle<B>[slangModule.bindings.size()],
			{descriptorSetLayoutDeleter, slangModule.bindings.size()});

		for (const auto& shader : slangModule.shaders)
		{
			auto createShaderModule = [](DeviceHandle<B> device, const ShaderEntry& shader)
			{
				ZoneScopedN("createShaderModule");

				VkShaderModuleCreateInfo info = {};
				info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
				info.codeSize = shader.first.size();
				info.pCode = reinterpret_cast<const uint32_t*>(shader.first.data());

				VkShaderModule vkShaderModule;
				CHECK_VK(vkCreateShaderModule(device, &info, nullptr, &vkShaderModule));

				return vkShaderModule;
			};

			pipelineLayout.shaders.get()[&shader - &slangModule.shaders[0]] =
				createShaderModule(device, shader);
		}

		size_t layoutIt = 0;
		for (auto& [space, layoutBindings] : slangModule.bindings)
		{
			pipelineLayout.descriptorSetLayouts.get()[layoutIt++] =
				createDescriptorSetLayout(device, layoutBindings);
		}

		VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = slangModule.bindings.size();
		pipelineLayoutInfo.pSetLayouts = pipelineLayout.descriptorSetLayouts.get();
		pipelineLayoutInfo.pushConstantRangeCount = 0;
		pipelineLayoutInfo.pPushConstantRanges = nullptr;

		CHECK_VK(vkCreatePipelineLayout(
			device, &pipelineLayoutInfo, nullptr, &pipelineLayout.layout));

		return pipelineLayout;
	};

	InstanceHandle<B> createInstance() const
	{
		ZoneScoped;

		VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "No Engine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_1;

		uint32_t instanceLayerCount;
		CHECK_VK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
		std::cout << instanceLayerCount << " layers found!\n";
		if (instanceLayerCount > 0)
		{
			std::unique_ptr<VkLayerProperties[]> instanceLayers(
				new VkLayerProperties[instanceLayerCount]);
			CHECK_VK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.get()));
			for (uint32_t i = 0; i < instanceLayerCount; ++i)
				std::cout << instanceLayers[i].layerName << "\n";
		}

		const char* enabledLayerNames[] =
		{
		#ifdef PROFILING_ENABLED
			"VK_LAYER_LUNARG_standard_validation",
		#endif
		};

		uint32_t instanceExtensionCount;
		vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);

		std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
		vkEnumerateInstanceExtensionProperties(
			nullptr, &instanceExtensionCount, availableInstanceExtensions.data());

		std::vector<const char*> instanceExtensions(instanceExtensionCount);
		for (uint32_t i = 0; i < instanceExtensionCount; i++)
		{
			instanceExtensions[i] = availableInstanceExtensions[i].extensionName;
			std::cout << instanceExtensions[i] << "\n";
		}

		std::sort(
			instanceExtensions.begin(), instanceExtensions.end(),
			[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

		std::vector<const char*> requiredExtensions = {
			// must be sorted lexicographically for std::includes to work!
			"VK_EXT_debug_report",
			"VK_KHR_surface",
#if defined(__WINDOWS__)
			"VK_KHR_win32_surface",
#elif defined(__APPLE__)
			"VK_MVK_macos_surface",
#elif defined(__linux__)
#	if defined(VK_USE_PLATFORM_XCB_KHR)
			"VK_KHR_xcb_surface",
#	elif defined(VK_USE_PLATFORM_XLIB_KHR)
			"VK_KHR_xlib_surface",
#	elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
			"VK_KHR_wayland_surface",
#	else // default to xcb
			"VK_KHR_xcb_surface",
#	endif
#endif
		};

		assert(std::includes(
			instanceExtensions.begin(), instanceExtensions.end(), requiredExtensions.begin(),
			requiredExtensions.end(),
			[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

		// if (std::find(instanceExtensions.begin(), instanceExtensions.end(), "VK_KHR_display") ==
		// instanceExtensions.end()) 	instanceExtensions.push_back("VK_KHR_display");

		VkInstanceCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		info.pApplicationInfo = &appInfo;
		info.enabledLayerCount = sizeof_array(enabledLayerNames);
		info.ppEnabledLayerNames = info.enabledLayerCount ? enabledLayerNames : nullptr;
		info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
		info.ppEnabledExtensionNames = requiredExtensions.data();

		VkInstance instance = VK_NULL_HANDLE;
		CHECK_VK(vkCreateInstance(&info, NULL, &instance));

		return instance;
	}

	SurfaceHandle<B> createSurface(InstanceHandle<B> instance, void* view) const
	{
		ZoneScoped;

		SurfaceHandle<B> surface;
		CHECK_VK(glfwCreateWindowSurface(
			instance, reinterpret_cast<GLFWwindow*>(view), nullptr, &surface));

		return surface;
	}

	std::tuple<
		DeviceHandle<B>, PhysicalDeviceHandle<B>, PhysicalDeviceProperties<B>, SwapchainInfo<B>, SurfaceFormat<B>, PresentMode<B>, uint32_t,
		QueueHandle<B>, int>
	createDevice(InstanceHandle<B> instance, SurfaceHandle<B> surface) const
	{
		ZoneScoped;

		DeviceHandle<B> logicalDevice;
		PhysicalDeviceHandle<B> physicalDevice;
		SwapchainInfo<B> swapChainInfo;
		SurfaceFormat<B> selectedSurfaceFormat;
		PresentMode<B> selectedPresentMode;
		uint32_t selectedFrameCount;
		QueueHandle<B> selectedQueue;
		int selectedQueueFamilyIndex = -1;
		PhysicalDeviceProperties<B> physicalDeviceProperties;

		uint32_t deviceCount = 0;
		CHECK_VK(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
		if (deviceCount == 0)
			throw std::runtime_error("failed to find GPUs with Vulkan support!");

		std::vector<VkPhysicalDevice> devices(deviceCount);
		CHECK_VK(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()));

		for (const auto& device : devices)
		{
			std::tie(swapChainInfo, selectedQueueFamilyIndex, physicalDeviceProperties) =
				getSuitableSwapchainAndQueueFamilyIndex(surface, device);

			if (selectedQueueFamilyIndex >= 0)
			{
				physicalDevice = device;

				const VkFormat requestSurfaceImageFormat[] = {
					VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM,
					VK_FORMAT_R8G8B8_UNORM};
				const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
				const VkPresentModeKHR requestPresentMode[] = {
					VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR,
					VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};

				// Request several formats, the first found will be used
				// If none of the requested image formats could be found, use the first available
				selectedSurfaceFormat = swapChainInfo.formats[0];
				for (uint32_t request_i = 0; request_i < sizeof_array(requestSurfaceImageFormat);
					 request_i++)
				{
					VkSurfaceFormatKHR requestedFormat = {requestSurfaceImageFormat[request_i],
														  requestSurfaceColorSpace};
					auto formatIt = std::find(
						swapChainInfo.formats.begin(), swapChainInfo.formats.end(),
						requestedFormat);
					if (formatIt != swapChainInfo.formats.end())
					{
						selectedSurfaceFormat = *formatIt;
						break;
					}
				}

				// Request a certain mode and confirm that it is available. If not use
				// VK_PRESENT_MODE_FIFO_KHR which is mandatory
				selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
				selectedFrameCount = 2;
				for (uint32_t request_i = 0; request_i < sizeof_array(requestPresentMode);
					 request_i++)
				{
					auto modeIt = std::find(
						swapChainInfo.presentModes.begin(), swapChainInfo.presentModes.end(),
						requestPresentMode[request_i]);
					if (modeIt != swapChainInfo.presentModes.end())
					{
						selectedPresentMode = *modeIt;

						if (selectedPresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
							selectedFrameCount = 3;

						break;
					}
				}

				break;
			}
		}

		if (physicalDevice == VK_NULL_HANDLE)
			throw std::runtime_error("failed to find a suitable GPU!");

		const float graphicsQueuePriority = 1.0f;

		VkDeviceQueueCreateInfo queueCreateInfo = {};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = selectedQueueFamilyIndex;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &graphicsQueuePriority;

		VkPhysicalDeviceFeatures deviceFeatures = {};
		deviceFeatures.samplerAnisotropy = VK_TRUE;

		uint32_t deviceExtensionCount;
		vkEnumerateDeviceExtensionProperties(
			physicalDevice, nullptr, &deviceExtensionCount, nullptr);

		std::vector<VkExtensionProperties> availableDeviceExtensions(deviceExtensionCount);
		vkEnumerateDeviceExtensionProperties(
			physicalDevice, nullptr, &deviceExtensionCount, availableDeviceExtensions.data());

		std::vector<const char*> deviceExtensions;
		deviceExtensions.reserve(deviceExtensionCount);
		for (uint32_t i = 0; i < deviceExtensionCount; i++)
		{
#if defined(__APPLE__)
			if (strcmp(availableDeviceExtensions[i].extensionName, "VK_MVK_moltenvk") == 0 ||
				strcmp(availableDeviceExtensions[i].extensionName, "VK_KHR_surface") == 0 ||
				strcmp(availableDeviceExtensions[i].extensionName, "VK_MVK_macos_surface") == 0)
				continue;
#endif
			deviceExtensions.push_back(availableDeviceExtensions[i].extensionName);
			std::cout << deviceExtensions.back() << "\n";
		}

		std::sort(
			deviceExtensions.begin(), deviceExtensions.end(),
			[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

		std::vector<const char*> requiredDeviceExtensions = {
			// must be sorted lexicographically for std::includes to work!
			"VK_KHR_swapchain"};

		assert(std::includes(
			deviceExtensions.begin(), deviceExtensions.end(), requiredDeviceExtensions.begin(),
			requiredDeviceExtensions.end(),
			[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

		VkDeviceCreateInfo deviceCreateInfo = {};
		deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
		deviceCreateInfo.enabledExtensionCount =
			static_cast<uint32_t>(requiredDeviceExtensions.size());
		deviceCreateInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

		CHECK_VK(vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &logicalDevice));

		vkGetDeviceQueue(logicalDevice, selectedQueueFamilyIndex, 0, &selectedQueue);

		return std::make_tuple(
			logicalDevice, physicalDevice, physicalDeviceProperties, swapChainInfo, selectedSurfaceFormat,
			selectedPresentMode, selectedFrameCount, selectedQueue, selectedQueueFamilyIndex);
	}

	void createSwapchain(
		DeviceHandle<B> device,
		PhysicalDeviceHandle<B> physicalDevice,
		VmaAllocator allocator,
		uint32_t frameCount,
		WindowData<B>& window) const
	{
		ZoneScoped;

		SwapchainHandle<B> newSwapchain;
		{
			VkSwapchainCreateInfoKHR info = {};
			info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
			info.surface = window.surface;
			info.minImageCount = frameCount;
			info.imageFormat = window.surfaceFormat.format;
			info.imageColorSpace = window.surfaceFormat.colorSpace;
			info.imageExtent = {window.framebufferWidth, window.framebufferHeight};
			info.imageArrayLayers = 1;
			info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			info.imageSharingMode =
				VK_SHARING_MODE_EXCLUSIVE; // Assume that graphics family == present family
			info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
			info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			info.presentMode = window.presentMode;
			info.clipped = VK_TRUE;
			info.oldSwapchain = window.swapchain.swapchain;

			CHECK_VK(vkCreateSwapchainKHR(device, &info, nullptr, &newSwapchain));
		}

		if (window.swapchain.swapchain)
			vkDestroySwapchainKHR(device, window.swapchain.swapchain, nullptr);

		window.swapchain.swapchain = newSwapchain;

		uint32_t imageCount;
		CHECK_VK(
			vkGetSwapchainImagesKHR(device, window.swapchain.swapchain, &imageCount, nullptr));
		
		window.swapchain.colorImages.resize(imageCount);
		window.swapchain.colorImageViews.resize(imageCount);
		
		CHECK_VK(vkGetSwapchainImagesKHR(
			device, window.swapchain.swapchain, &imageCount,
			window.swapchain.colorImages.data()));

		// todo: append stencil bit for depthstencil composite formats
		window.zBuffer = std::make_shared<Texture<B>>(
			myDevice, myTransferCommandPool, myQueue, myAllocator,
			nullptr, window.framebufferWidth, window.framebufferHeight,
			findSupportedFormat(
				physicalDevice,
				{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
				VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
			"zBuffer");
	}

	VmaAllocator createAllocator(DeviceHandle<B> device, PhysicalDeviceHandle<B> physicalDevice) const
	{
		ZoneScoped;

		auto vkGetBufferMemoryRequirements2KHR =
			(PFN_vkGetBufferMemoryRequirements2KHR)vkGetInstanceProcAddr(
				myInstance, "vkGetBufferMemoryRequirements2KHR");
		assert(vkGetBufferMemoryRequirements2KHR != nullptr);

		auto vkGetImageMemoryRequirements2KHR =
			(PFN_vkGetImageMemoryRequirements2KHR)vkGetInstanceProcAddr(
				myInstance, "vkGetImageMemoryRequirements2KHR");
		assert(vkGetImageMemoryRequirements2KHR != nullptr);

		VmaVulkanFunctions functions = {};
		functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
		functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
		functions.vkAllocateMemory = vkAllocateMemory;
		functions.vkFreeMemory = vkFreeMemory;
		functions.vkMapMemory = vkMapMemory;
		functions.vkUnmapMemory = vkUnmapMemory;
		functions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
		functions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
		functions.vkBindBufferMemory = vkBindBufferMemory;
		functions.vkBindImageMemory = vkBindImageMemory;
		functions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
		functions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
		functions.vkCreateBuffer = vkCreateBuffer;
		functions.vkDestroyBuffer = vkDestroyBuffer;
		functions.vkCreateImage = vkCreateImage;
		functions.vkDestroyImage = vkDestroyImage;
		functions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2KHR;
		functions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2KHR;

		VmaAllocator allocator;
		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.physicalDevice = physicalDevice;
		allocatorInfo.device = device;
		allocatorInfo.pVulkanFunctions = &functions;
		vmaCreateAllocator(&allocatorInfo, &allocator);

		return allocator;
	}

	DescriptorPoolHandle<B> createDescriptorPool() const
	{
		ZoneScoped;

		constexpr uint32_t maxDescriptorCount = 1000;
		VkDescriptorPoolSize poolSizes[] = {
			{VK_DESCRIPTOR_TYPE_SAMPLER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, maxDescriptorCount}};

		VkDescriptorPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.poolSizeCount = static_cast<uint32_t>(sizeof_array(poolSizes));
		poolInfo.pPoolSizes = poolSizes;
		poolInfo.maxSets = maxDescriptorCount * static_cast<uint32_t>(sizeof_array(poolSizes));
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

		DescriptorPoolHandle<B> outDescriptorPool;
		CHECK_VK(vkCreateDescriptorPool(myDevice, &poolInfo, nullptr, &outDescriptorPool));

		return outDescriptorPool;
	}

	RenderPassHandle<B> createRenderPass(DeviceHandle<B> device, const WindowData<B>& window) const
	{
		ZoneScoped;

		VkAttachmentDescription colorAttachment = {};
		colorAttachment.format = window.surfaceFormat.format;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentDescription depthAttachment = {};
		depthAttachment.format = window.zBuffer->format;
		depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colorAttachmentRef = {};
		colorAttachmentRef.attachment = 0;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference depthAttachmentRef = {};
		depthAttachmentRef.attachment = 1;
		depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorAttachmentRef;
		subpass.pDepthStencilAttachment = &depthAttachmentRef;

		VkSubpassDependency dependency = {};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
		VkRenderPassCreateInfo renderPassInfo = {};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		RenderPassHandle<B> outRenderPass;
		CHECK_VK(vkCreateRenderPass(device, &renderPassInfo, nullptr, &outRenderPass));

		return outRenderPass;
	}

	PipelineHandle<B> createGraphicsPipeline(
		DeviceHandle<B> device,
		const PipelineConfiguration<B>& pipeline,
		PipelineCacheHandle<B>& pipelineCache,
		const Model<B>& model) const
	{
		ZoneScoped;

		VkPipelineShaderStageCreateInfo vsStageInfo = {};
		vsStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		vsStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
		vsStageInfo.module = pipeline.layout->shaders[0];
		vsStageInfo.pName = "main"; // todo: get from named VkShaderModule object

		// struct AlphaTestSpecializationData
		// {
		// 	uint32_t alphaTestMethod = 0;
		// 	float alphaTestRef = 0.5f;
		// } alphaTestSpecializationData;

		// std::array<VkSpecializationMapEntry, 2> alphaTestSpecializationMapEntries;
		// alphaTestSpecializationMapEntries[0].constantID = 0;
		// alphaTestSpecializationMapEntries[0].size =
		// sizeof(alphaTestSpecializationData.alphaTestMethod);
		// alphaTestSpecializationMapEntries[0].offset = 0;
		// alphaTestSpecializationMapEntries[1].constantID = 1;
		// alphaTestSpecializationMapEntries[1].size =
		// sizeof(alphaTestSpecializationData.alphaTestRef);
		// alphaTestSpecializationMapEntries[1].offset =
		// offsetof(AlphaTestSpecializationData, alphaTestRef);

		// VkSpecializationInfo specializationInfo = {};
		// specializationInfo.dataSize = sizeof(alphaTestSpecializationData);
		// specializationInfo.mapEntryCount =
		// static_cast<uint32_t>(alphaTestSpecializationMapEntries.size());
		// specializationInfo.pMapEntries = alphaTestSpecializationMapEntries.data();
		// specializationInfo.pData = &alphaTestSpecializationData;

		VkPipelineShaderStageCreateInfo fsStageInfo = {};
		fsStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		fsStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		fsStageInfo.module = pipeline.layout->shaders[1];
		fsStageInfo.pName = "main";
		fsStageInfo.pSpecializationInfo = nullptr; //&specializationInfo;

		VkPipelineShaderStageCreateInfo shaderStages[] = {vsStageInfo, fsStageInfo};

		VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputInfo.vertexBindingDescriptionCount = model.bindings.size();
		vertexInputInfo.pVertexBindingDescriptions = model.bindings.data();
		vertexInputInfo.vertexAttributeDescriptionCount =
			static_cast<uint32_t>(model.attributes.size());
		vertexInputInfo.pVertexAttributeDescriptions = model.attributes.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		inputAssembly.primitiveRestartEnable = VK_FALSE;

		VkViewport viewport = {};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = static_cast<float>(pipeline.resources->window->framebufferWidth);
		viewport.height = static_cast<float>(pipeline.resources->window->framebufferHeight);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;

		VkRect2D scissor = {};
		scissor.offset = {0, 0};
		scissor.extent = {
			static_cast<uint32_t>(pipeline.resources->window->framebufferWidth),
			static_cast<uint32_t>(pipeline.resources->window->framebufferHeight)};

		VkPipelineViewportStateCreateInfo viewportState = {};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.pViewports = &viewport;
		viewportState.scissorCount = 1;
		viewportState.pScissors = &scissor;

		VkPipelineRasterizationStateCreateInfo rasterizer = {};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.depthClampEnable = VK_FALSE;
		rasterizer.rasterizerDiscardEnable = VK_FALSE;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.lineWidth = 1.0f;
		rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
		rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterizer.depthBiasEnable = VK_FALSE;
		rasterizer.depthBiasConstantFactor = 0.0f;
		rasterizer.depthBiasClamp = 0.0f;
		rasterizer.depthBiasSlopeFactor = 0.0f;

		VkPipelineMultisampleStateCreateInfo multisampling = {};
		multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampling.sampleShadingEnable = VK_FALSE;
		multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		multisampling.minSampleShading = 1.0f;
		multisampling.pSampleMask = nullptr;
		multisampling.alphaToCoverageEnable = VK_FALSE;
		multisampling.alphaToOneEnable = VK_FALSE;

		VkPipelineDepthStencilStateCreateInfo depthStencil = {};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = VK_TRUE;
		depthStencil.depthWriteEnable = VK_TRUE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
		depthStencil.depthBoundsTestEnable = VK_FALSE;
		depthStencil.minDepthBounds = 0.0f;
		depthStencil.maxDepthBounds = 1.0f;
		depthStencil.stencilTestEnable = VK_FALSE;
		depthStencil.front = {};
		depthStencil.back = {};

		VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
		colorBlendAttachment.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachment.blendEnable = VK_FALSE;
		colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

		VkPipelineColorBlendStateCreateInfo colorBlending = {};
		colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlending.logicOpEnable = VK_FALSE;
		colorBlending.logicOp = VK_LOGIC_OP_COPY;
		colorBlending.attachmentCount = 1;
		colorBlending.pAttachments = &colorBlendAttachment;
		colorBlending.blendConstants[0] = 0.0f;
		colorBlending.blendConstants[1] = 0.0f;
		colorBlending.blendConstants[2] = 0.0f;
		colorBlending.blendConstants[3] = 0.0f;

		std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
														VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamicState = {};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
		dynamicState.pDynamicStates = dynamicStates.data();

		VkGraphicsPipelineCreateInfo pipelineInfo = {};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = sizeof_array(shaderStages);
		pipelineInfo.pStages = shaderStages;
		pipelineInfo.pVertexInputState = &vertexInputInfo;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pDepthStencilState = &depthStencil;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.layout = pipeline.layout->layout;
		pipelineInfo.renderPass = pipeline.renderPass;
		pipelineInfo.subpass = 0;
		pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
		pipelineInfo.basePipelineIndex = -1;

		PipelineHandle<B> outPipeline;

		CHECK_VK(vkCreateGraphicsPipelines(
			device, pipelineCache, 1, &pipelineInfo, nullptr, &outPipeline));

		return outPipeline;
	};

	 // temp - replace with proper code
	void createPipelineConfig(DeviceHandle<B> device, const Model<B>& model)
	{
		ZoneScoped;

		myGraphicsPipelineConfig.resources = myDefaultResources;
		myGraphicsPipelineConfig.layout = myGraphicsPipelineLayout;
	}

	void initIMGUI(WindowData<B>& window, float dpiScaleX, float dpiScaleY) const
	{
		ZoneScoped;

		using namespace ImGui;

		IMGUI_CHECKVERSION();
		CreateContext();
		auto& io = GetIO();
		// io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		// io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

		io.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

		ImFontConfig config;
		config.OversampleH = 2;
		config.OversampleV = 2;
		config.PixelSnapH = false;

		io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

		std::filesystem::path fontPath(myResourcePath);
		fontPath = std::filesystem::absolute(fontPath);
		fontPath /= "fonts";
		fontPath /= "foo";

		const char* fonts[] = {
			"Cousine-Regular.ttf", "DroidSans.ttf",  "Karla-Regular.ttf",
			"ProggyClean.ttf",	 "ProggyTiny.ttf", "Roboto-Medium.ttf",
		};

		ImFont* defaultFont = nullptr;
		for (auto font : fonts)
		{
			fontPath.replace_filename(font);
			defaultFont = io.Fonts->AddFontFromFileTTF(
				fontPath.u8string().c_str(), 16.0f,
				&config);
		}

		// Setup style
		StyleColorsClassic();
		io.FontDefault = defaultFont;

		// Setup Vulkan binding
		ImGui_ImplVulkan_InitInfo initInfo = {};
		initInfo.Instance = myInstance;
		initInfo.PhysicalDevice = myPhysicalDevice;
		initInfo.Device = myDevice;
		initInfo.QueueFamily = myQueueFamilyIndex;
		initInfo.Queue = myQueue;
		initInfo.PipelineCache = VK_NULL_HANDLE;
		initInfo.DescriptorPool = myDescriptorPool;
		initInfo.MinImageCount = window.swapchain.colorImages.size();
		initInfo.ImageCount = window.swapchain.colorImages.size();
		initInfo.Allocator = nullptr; // myAllocator;
		// initInfo.HostAllocationCallbacks = nullptr;
		initInfo.CheckVkResultFn = CHECK_VK;
		ImGui_ImplVulkan_Init(&initInfo, myGraphicsPipelineConfig.renderPass);

		// Upload Fonts
		{
			VkCommandBuffer commandBuffer = beginSingleTimeCommands(myDevice, myTransferCommandPool);

			//TracyVkZone(myTracyContext, commandBuffer, "uploadFontTexture");

			ImGui_ImplVulkan_CreateFontsTexture(commandBuffer);
			endSingleTimeCommands(myDevice, myQueue, commandBuffer, myTransferCommandPool);
			ImGui_ImplVulkan_DestroyFontUploadObjects();
		}
	}

	void createFrameResources(
		WindowData<B>& window,
		const std::vector<Model<B>*>& models)
	{
		ZoneScoped;

		window.views.resize(NX * NY);
		for (auto& view : window.views)
		{
			if (!view.viewport.width)
				view.viewport.width = window.framebufferWidth / NX;

			if (!view.viewport.height)
				view.viewport.height = window.framebufferHeight / NY;

			updateViewMatrix(view);
			updateProjectionMatrix(view);
		}

		myGraphicsPipelineConfig.renderPass = createRenderPass(myDevice, window);

		auto createSwapchainImageViews = [this, &window]() {
			ZoneScopedN("createSwapchainImageViews");

			for (uint32_t i = 0; i < window.swapchain.colorImages.size(); i++)
				window.swapchain.colorImageViews[i] = createImageView2D(
					myDevice, window.swapchain.colorImages[i], window.surfaceFormat.format,
					VK_IMAGE_ASPECT_COLOR_BIT);
		};

		createSwapchainImageViews();

		for (auto& model : models)
		{
			createPipelineConfig(myDevice, *model);
			myGraphicsPipeline = createGraphicsPipeline(myDevice, myGraphicsPipelineConfig, myPipelineCache, *model);
			myDescriptorSets = allocateDescriptorSets(
				myDevice, myDescriptorPool, myGraphicsPipelineLayout->descriptorSetLayouts.get(),
				myGraphicsPipelineLayout->descriptorSetLayouts.get_deleter().size);
		}

		window.clearEnable = true;
		window.clearValue.color.float32[0] = 0.4f;
		window.clearValue.color.float32[1] = 0.4f;
		window.clearValue.color.float32[2] = 0.5f;
		window.clearValue.color.float32[3] = 1.0f;

		myFrameCommandPools.resize(myCommandBufferThreadCount);
		for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
		{
			myFrameCommandPools[threadIt] = createCommandPool(
				myDevice,
				VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
					VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
				myQueueFamilyIndex);
		}

		auto createFramebuffer = [this, &window](uint frameIndex)
		{
			std::array<VkImageView, 2> attachments = {window.swapchain.colorImageViews[frameIndex], window.zBuffer->imageView};
			VkFramebufferCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			info.renderPass = myGraphicsPipelineConfig.renderPass;
			info.attachmentCount = attachments.size();
			info.pAttachments = attachments.data();
			info.width = window.framebufferWidth;
			info.height = window.framebufferHeight;
			info.layers = 1;

			VkFramebuffer outFramebuffer = VK_NULL_HANDLE;
			CHECK_VK(vkCreateFramebuffer(myDevice, &info, nullptr, &outFramebuffer));

			return outFramebuffer;
		};

		window.frames.resize(window.swapchain.colorImages.size());
		for (uint32_t frameIt = 0; frameIt < window.frames.size(); frameIt++)
		{
			auto& frame = window.frames[frameIt];

			frame.index = frameIt;
			frame.frameBuffer = createFramebuffer(frameIt);
			frame.commandBuffers.resize(myCommandBufferThreadCount);

			for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
			{
				VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				cmdInfo.commandPool = myFrameCommandPools[threadIt];
				cmdInfo.level = threadIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
				cmdInfo.commandBufferCount = 1;
				CHECK_VK(vkAllocateCommandBuffers(myDevice, &cmdInfo, &frame.commandBuffers[threadIt]));
			}

			VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			CHECK_VK(vkCreateFence(myDevice, &fenceInfo, nullptr, &frame.fence));

			VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			CHECK_VK(vkCreateSemaphore(
				myDevice, &semaphoreInfo, nullptr, &frame.renderCompleteSemaphore));
			CHECK_VK(vkCreateSemaphore(
				myDevice, &semaphoreInfo, nullptr, &frame.newImageAcquiredSemaphore));

			frame.graphicsFrameTimestamp = std::chrono::high_resolution_clock::now();
		}

		updateDescriptorSets(window);

		// vkAcquireNextImageKHR uses semaphore from last frame -> cant use index 0 for first frame
		window.frameIndex = window.frames.size() - 1;

		// todo: set up on transfer commandlist
		myTracyContext = TracyVkContext(myPhysicalDevice, myDevice, myQueue, window.frames[0].commandBuffers[0]);
	}

	void checkFlipOrPresentResult(WindowData<B>& window, VkResult result) const
	{
		switch (result)
		{
		case VK_SUCCESS:
			break;
		case VK_SUBOPTIMAL_KHR:
			std::cout << "warning: flip/present returned VK_SUBOPTIMAL_KHR";
			break;
		case VK_ERROR_OUT_OF_DATE_KHR:
			window.createFrameResourcesFlag = true;
			break;
		default:
			throw std::runtime_error("failed to flip swap chain image!");
		}
	}

	void updateViewMatrix(ViewData& view) const
	{
		auto Rx = glm::rotate(glm::mat4(1.0), view.camRot.x, glm::vec3(-1, 0, 0));
		auto Ry = glm::rotate(glm::mat4(1.0), view.camRot.y, glm::vec3(0, -1, 0));
		auto T = glm::translate(glm::mat4(1.0), -view.camPos);

		view.view = glm::inverse(T * Ry * Rx);
	}

	void updateProjectionMatrix(ViewData& view) const
	{
		constexpr glm::mat4 clip = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
									0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f,  0.5f, 1.0f};

		constexpr auto fov = 75.0f;
		auto aspect =
			static_cast<float>(view.viewport.width) / static_cast<float>(view.viewport.height);
		constexpr auto nearplane = 0.01f;
		constexpr auto farplane = 100.0f;

		view.projection = clip * glm::perspective(glm::radians(fov), aspect, nearplane, farplane);
	}

	void updateUniformBuffers(WindowData<B>& window) const
	{
		ZoneScoped;

		void* data;
		CHECK_VK(vmaMapMemory(myAllocator, window.uniformBufferMemory, &data));

		for (uint32_t n = 0; n < (NX * NY); n++)
		{
			UniformBufferObject& ubo = reinterpret_cast<UniformBufferObject*>(data)[window.frameIndex * (NX * NY) + n];

			ubo.model = glm::mat4(1.0f); // myDefaultResources->model.transform;
			ubo.view = glm::mat4(window.views[n].view);
			ubo.proj = window.views[n].projection;
		}

		vmaFlushAllocation(
			myAllocator,
			window.uniformBufferMemory,
			sizeof(UniformBufferObject) * window.frameIndex * (NX * NY),
			sizeof(UniformBufferObject) * (NX * NY));

		vmaUnmapMemory(myAllocator, window.uniformBufferMemory);
	}

	void updateDescriptorSets(WindowData<B>& window) const
	{
		ZoneScoped;

		VkDescriptorBufferInfo bufferInfo = {};
		bufferInfo.buffer = window.uniformBuffer;
		bufferInfo.offset = 0;
		bufferInfo.range = VK_WHOLE_SIZE;

		VkDescriptorImageInfo imageInfo = {};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = myDefaultResources->texture->imageView;
		imageInfo.sampler = myDefaultResources->sampler;

		std::array<VkWriteDescriptorSet, 3> descriptorWrites = {};
		descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[0].dstSet = myDescriptorSets[0];
		descriptorWrites[0].dstBinding = 0;
		descriptorWrites[0].dstArrayElement = 0;
		descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		descriptorWrites[0].descriptorCount = 1;
		descriptorWrites[0].pBufferInfo = &bufferInfo;
		descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[1].dstSet = myDescriptorSets[0];
		descriptorWrites[1].dstBinding = 1;
		descriptorWrites[1].dstArrayElement = 0;
		descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptorWrites[1].descriptorCount = 1;
		descriptorWrites[1].pImageInfo = &imageInfo;
		descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[2].dstSet = myDescriptorSets[0];
		descriptorWrites[2].dstBinding = 2;
		descriptorWrites[2].dstArrayElement = 0;
		descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		descriptorWrites[2].descriptorCount = 1;
		descriptorWrites[2].pImageInfo = &imageInfo;

		vkUpdateDescriptorSets(
			myDevice, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(),
			0, nullptr);
	}

	void submitFrame(WindowData<B>& window)
	{
		ZoneScoped;

		window.lastFrameIndex = window.frameIndex;
		auto& lastFrame = window.frames[window.lastFrameIndex];

		{
			ZoneScopedN("acquireNextImage");

			checkFlipOrPresentResult(window, vkAcquireNextImageKHR(
				myDevice, window.swapchain.swapchain, UINT64_MAX, lastFrame.newImageAcquiredSemaphore, VK_NULL_HANDLE,
				&window.frameIndex));
		}

		auto& frame = window.frames[window.frameIndex];
		
		// wait for frame to be completed before starting to use it
		{
			ZoneScopedN("waitForFrameFence");

			CHECK_VK(vkWaitForFences(myDevice, 1, &frame.fence, VK_TRUE, UINT64_MAX));
			CHECK_VK(vkResetFences(myDevice, 1, &frame.fence));

			frame.graphicsFrameTimestamp = std::chrono::high_resolution_clock::now();
			frame.graphicsDeltaTime = frame.graphicsFrameTimestamp - lastFrame.graphicsFrameTimestamp;
		}

		std::future<void> updateUniformBuffersFuture(std::async(std::launch::async, [this, &window]
		{
			updateUniformBuffers(window);
		}));

		std::future<void> secondaryCommandsFuture(std::async(std::launch::async, [this, &window, &frame]
		{
			// setup draw parameters
			constexpr uint32_t drawCount = NX * NY;
			uint32_t segmentCount = std::max(myCommandBufferThreadCount - 1u, 1u);

			assert(myGraphicsPipelineConfig.resources != nullptr);
			auto& resources = *myGraphicsPipelineConfig.resources;

			// begin secondary command buffers
			for (uint32_t segmentIt = 0; segmentIt < segmentCount; segmentIt++)
			{
				ZoneScopedN("beginSecondaryCommands");

				VkCommandBuffer cmd = frame.commandBuffers[segmentIt + 1];

				VkCommandBufferInheritanceInfo inherit = {
					VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
				inherit.renderPass = myGraphicsPipelineConfig.renderPass;
				inherit.framebuffer = frame.frameBuffer;

				CHECK_VK(vkResetCommandBuffer(cmd, 0));
				VkCommandBufferBeginInfo secBeginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
				secBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
									VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
									VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
				secBeginInfo.pInheritanceInfo = &inherit;
				CHECK_VK(vkBeginCommandBuffer(cmd, &secBeginInfo));
			}

			// draw geometry using secondary command buffers
			{
				ZoneScopedN("draw");

				uint32_t segmentDrawCount = drawCount / segmentCount;
				if (drawCount % segmentCount)
					segmentDrawCount += 1;

				uint32_t dx = window.framebufferWidth / NX;
				uint32_t dy = window.framebufferHeight / NY;

				std::array<uint32_t, 128> seq;
				std::iota(seq.begin(), seq.begin() + segmentCount, 0);
				std::for_each_n(
	// #if defined(__WINDOWS__)
	// 				std::execution::par,
	// #endif
					seq.begin(), segmentCount,
					[this, &resources, &dx, &dy, &segmentDrawCount, &frame](uint32_t segmentIt) {
						ZoneScopedN("drawSegment");

						VkCommandBuffer cmd = frame.commandBuffers[segmentIt + 1];

						//TracyVkZone(myTracyContext, cmd, "draw");

						// bind pipeline and inputs
						{
							// bind pipeline and vertex/index buffers
							vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, myGraphicsPipeline);

							VkBuffer vertexBuffers[] = {resources.model->vertexBuffer};
							VkDeviceSize vertexOffsets[] = {0};

							vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
							vkCmdBindIndexBuffer(cmd, resources.model->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
						}

						for (uint32_t drawIt = 0; drawIt < segmentDrawCount; drawIt++)
						{
							//TracyVkZone(myTracyContext, cmd, "drawModel");
							
							uint32_t n = segmentIt * segmentDrawCount + drawIt;

							if (n >= drawCount)
								break;

							uint32_t i = n % NX;
							uint32_t j = n / NX;

							auto setViewportAndScissor = [](VkCommandBuffer cmd, int32_t x, int32_t y,
															int32_t width, int32_t height) {
								VkViewport viewport = {};
								viewport.x = static_cast<float>(x);
								viewport.y = static_cast<float>(y);
								viewport.width = static_cast<float>(width);
								viewport.height = static_cast<float>(height);
								viewport.minDepth = 0.0f;
								viewport.maxDepth = 1.0f;

								VkRect2D scissor = {};
								scissor.offset = {x, y};
								scissor.extent = {static_cast<uint32_t>(width),
												static_cast<uint32_t>(height)};

								vkCmdSetViewport(cmd, 0, 1, &viewport);
								vkCmdSetScissor(cmd, 0, 1, &scissor);
							};

							auto drawModel = [&n, &frame](
												VkCommandBuffer cmd, uint32_t indexCount,
												uint32_t descriptorSetCount,
												const VkDescriptorSet* descriptorSets,
												VkPipelineLayout pipelineLayout) {
								uint32_t uniformBufferOffset = (frame.index * drawCount + n) * sizeof(UniformBufferObject);
								vkCmdBindDescriptorSets(
									cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
									descriptorSetCount, descriptorSets, 1, &uniformBufferOffset);
								vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
							};

							setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

							drawModel(
								cmd, myDefaultResources->model->indexCount, myDescriptorSets.size(),
								myDescriptorSets.data(), myGraphicsPipelineLayout->layout);
						}
					});
			}

			// end secondary command buffers
			for (uint32_t segmentIt = 0; segmentIt < segmentCount; segmentIt++)
			{
				VkCommandBuffer cmd = frame.commandBuffers
						[segmentIt + 1];
				{
					ZoneScopedN("endSecondaryCommands");

					CHECK_VK(vkEndCommandBuffer(cmd));
				}
			}
		}));

		if (window.imguiEnable)
			drawIMGUI(window);
		
		// begin primary command buffer
		{
			ZoneScopedN("beginCommands");

			CHECK_VK(vkResetCommandBuffer(frame.commandBuffers[0], 0));
			VkCommandBufferBeginInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			CHECK_VK(vkBeginCommandBuffer(frame.commandBuffers[0], &info));
		}

		// collect timing scopes
		{
			ZoneScopedN("tracyVkCollect");

			TracyVkZone(myTracyContext, frame.commandBuffers[0], "tracyVkCollect");

			TracyVkCollect(myTracyContext, frame.commandBuffers[0]);
		}

		std::array<ClearValue<B>, 2> clearValues = {};
		clearValues[0] = window.clearValue;
		clearValues[1].depthStencil = {1.0f, 0};

		// call secondary command buffers
		{
			{
				ZoneScopedN("waitSecondaryCommands");

				secondaryCommandsFuture.get();
			}

			ZoneScopedN("executeCommands");

			TracyVkZone(myTracyContext, frame.commandBuffers[0], "executeCommands");

			VkRenderPassBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			beginInfo.renderPass = myGraphicsPipelineConfig.renderPass;
			beginInfo.framebuffer = frame.frameBuffer;
			beginInfo.renderArea.offset = {0, 0};
			beginInfo.renderArea.extent = {static_cast<uint32_t>(window.framebufferWidth),
										   static_cast<uint32_t>(window.framebufferHeight)};
			beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
			beginInfo.pClearValues = clearValues.data();
			vkCmdBeginRenderPass(
				frame.commandBuffers[0], &beginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

			vkCmdExecuteCommands(frame.commandBuffers[0], (myCommandBufferThreadCount - 1), &frame.commandBuffers[1]);

			vkCmdEndRenderPass(frame.commandBuffers[0]);
		}

		// Record Imgui Draw Data and draw funcs into primary command buffer
		if (window.imguiEnable)
		{
			ZoneScopedN("drawIMGUI");

			TracyVkZone(myTracyContext, frame.commandBuffers[0], "drawIMGUI");

			VkRenderPassBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			beginInfo.renderPass = myGraphicsPipelineConfig.renderPass;
			beginInfo.framebuffer = frame.frameBuffer;
			beginInfo.renderArea.offset = {0, 0};
			beginInfo.renderArea.extent.width = window.framebufferWidth;
			beginInfo.renderArea.extent.height = window.framebufferHeight;
			beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
			beginInfo.pClearValues = clearValues.data();
			vkCmdBeginRenderPass(frame.commandBuffers[0], &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commandBuffers[0]);

			vkCmdEndRenderPass(frame.commandBuffers[0]);
		}

		// Submit primary command buffer
		{
			{
				ZoneScopedN("waitFramePrepare");

				updateUniformBuffersFuture.get();
				
			}

			ZoneScopedN("submitCommands");

			VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			VkSubmitInfo submitInfo = {};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.waitSemaphoreCount = 1;
			submitInfo.pWaitSemaphores = &lastFrame.newImageAcquiredSemaphore;
			submitInfo.pWaitDstStageMask = &waitStage;
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers = &frame.commandBuffers[0];
			submitInfo.signalSemaphoreCount = 1;
			submitInfo.pSignalSemaphores = &frame.renderCompleteSemaphore;

			CHECK_VK(vkEndCommandBuffer(frame.commandBuffers[0]));
			CHECK_VK(vkQueueSubmit(myQueue, 1, &submitInfo, frame.fence));
		}
	}

	void presentFrame(WindowData<B>& window) const
	{
		ZoneScoped;

		auto& frame = window.frames[window.frameIndex];

		VkPresentInfoKHR info = {};
		info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &frame.renderCompleteSemaphore;
		info.swapchainCount = 1;
		info.pSwapchains = &window.swapchain.swapchain;
		info.pImageIndices = &window.frameIndex;
		checkFlipOrPresentResult(window, vkQueuePresentKHR(myQueue, &info));
	}

	void cleanupFrameResources()
	{
		ZoneScoped;

		auto& window = *myDefaultResources->window;

		for (auto& frame : window.frames)
		{
			vkDestroyFence(myDevice, frame.fence, nullptr);
			vkDestroySemaphore(myDevice, frame.renderCompleteSemaphore, nullptr);
			vkDestroySemaphore(myDevice, frame.newImageAcquiredSemaphore, nullptr);

			for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
				vkFreeCommandBuffers(myDevice, myFrameCommandPools[threadIt], 1, &frame.commandBuffers[threadIt]);

			vkDestroyFramebuffer(myDevice, frame.frameBuffer, nullptr);
		}

		for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
			vkDestroyCommandPool(myDevice, myFrameCommandPools[threadIt], nullptr);	

		for (VkImageView imageView : window.swapchain.colorImageViews)
			vkDestroyImageView(myDevice, imageView, nullptr);

		vkDestroyRenderPass(myDevice, myGraphicsPipelineConfig.renderPass, nullptr);

		vkDestroyPipeline(myDevice, myGraphicsPipeline, nullptr);

		TracyVkDestroy(myTracyContext);
	}

	void cleanup()
	{
		ZoneScoped;

		cleanupFrameResources();

		ImGui_ImplVulkan_Shutdown();
		ImGui::DestroyContext();

		auto& window = *myDefaultResources->window;

		vkDestroySwapchainKHR(myDevice, window.swapchain.swapchain, nullptr);
		vmaDestroyBuffer(myAllocator, window.uniformBuffer, window.uniformBufferMemory);

		{
			window.zBuffer.reset();
			myDefaultResources->model.reset();
			myDefaultResources->texture.reset();
		}

		vkDestroyPipelineCache(myDevice, myPipelineCache, nullptr);
		vkDestroyPipelineLayout(myDevice, myGraphicsPipelineLayout->layout, nullptr);

		// todo: wrap these in a deleter.
		myGraphicsPipelineLayout->shaders.reset();
		myGraphicsPipelineLayout->descriptorSetLayouts.reset();

		vkDestroySampler(myDevice, myDefaultResources->sampler, nullptr);

		vkDestroyDescriptorPool(myDevice, myDescriptorPool, nullptr);
		vkDestroyCommandPool(myDevice, myTransferCommandPool, nullptr);

		char* allocatorStatsJSON = nullptr;
		vmaBuildStatsString(myAllocator, &allocatorStatsJSON, true);
		std::cout << allocatorStatsJSON << std::endl;
		vmaFreeStatsString(myAllocator, allocatorStatsJSON);
		vmaDestroyAllocator(myAllocator);

		vkDestroyDevice(myDevice, nullptr);
		vkDestroySurfaceKHR(myInstance, window.surface, nullptr);

		auto vkDestroyDebugReportCallbackEXT =
			(PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(
				myInstance, "vkDestroyDebugReportCallbackEXT");
		assert(vkDestroyDebugReportCallbackEXT != nullptr);
		vkDestroyDebugReportCallbackEXT(myInstance, myDebugCallback, nullptr);

		vkDestroyInstance(myInstance, nullptr);
	}

	struct UniformBufferObject
	{
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;
		glm::mat4 pad;
	};

	InstanceHandle<B> myInstance = 0;

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

	static constexpr uint32_t NX = 2;
	static constexpr uint32_t NY = 1;

	std::shared_ptr<GraphicsPipelineResourceContext<B>> myDefaultResources;
	std::shared_ptr<PipelineLayoutContext<B>> myGraphicsPipelineLayout;
	PipelineConfiguration<B> myGraphicsPipelineConfig = {};
	PipelineHandle<B> myGraphicsPipeline = 0; // ~ "PSO"
	PipelineCacheHandle<B> myPipelineCache = 0;

	using DescriptorSetVector = std::vector<DescriptorSetHandle<B>>;
	DescriptorSetVector myDescriptorSets;

	std::filesystem::path myResourcePath;

	uint32_t myCommandBufferThreadCount = 0;
	int myRequestedCommandBufferThreadCount = 0;

	std::map<int, bool> myKeysPressed;
	std::array<bool, 2> myMouseButtonsPressed;
	std::array<glm::vec2, 2> myMousePosition;
};

static Application<GraphicsBackend::Vulkan>* theApp = nullptr;

int vkapp_create(
	void* view, int width, int height, int framebufferWidth, int framebufferHeight,
	const char* resourcePath)
{
	assert(view != nullptr);
	assert(theApp == nullptr);

	static const char* DISABLE_VK_LAYER_VALVE_steam_overlay_1 =
		"DISABLE_VK_LAYER_VALVE_steam_overlay_1=1";
#if defined(__WINDOWS__)
	_putenv((char*)DISABLE_VK_LAYER_VALVE_steam_overlay_1);
#else
	putenv((char*)DISABLE_VK_LAYER_VALVE_steam_overlay_1);
#endif

#ifdef _DEBUG
	static const char* VK_LOADER_DEBUG_STR = "VK_LOADER_DEBUG";
	if (char* vkLoaderDebug = getenv(VK_LOADER_DEBUG_STR))
		std::cout << VK_LOADER_DEBUG_STR << "=" << vkLoaderDebug << std::endl;

	static const char* VK_LAYER_PATH_STR = "VK_LAYER_PATH";
	if (char* vkLayerPath = getenv(VK_LAYER_PATH_STR))
		std::cout << VK_LAYER_PATH_STR << "=" << vkLayerPath << std::endl;

	static const char* VK_ICD_FILENAMES_STR = "VK_ICD_FILENAMES";
	if (char* vkIcdFilenames = getenv(VK_ICD_FILENAMES_STR))
		std::cout << VK_ICD_FILENAMES_STR << "=" << vkIcdFilenames << std::endl;
#endif

	theApp = new Application<GraphicsBackend::Vulkan>(
		view, width, height, framebufferWidth, framebufferHeight,
		resourcePath ? resourcePath : "./");

	return EXIT_SUCCESS;
}

void vkapp_draw()
{
	assert(theApp != nullptr);

	theApp->draw();
}

void vkapp_resizeWindow(const window_state* state)
{
	assert(theApp != nullptr);
	assert(state != nullptr);

	theApp->resizeWindow(*state);
}

void vkapp_resizeFramebuffer(int width, int height)
{
	assert(theApp != nullptr);

	theApp->resizeFramebuffer(width, height);
}

void vkapp_mouse(const mouse_state* state)
{
	assert(theApp != nullptr);
	assert(state != nullptr);

	theApp->onMouse(*state);
}

void vkapp_keyboard(const keyboard_state* state)
{
	assert(theApp != nullptr);
	assert(state != nullptr);

	theApp->onKeyboard(*state);
}

void vkapp_destroy()
{
	assert(theApp != nullptr);

	delete theApp;
}
