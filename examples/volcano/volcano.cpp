// wip: separate IMGUI and volcano abstractions more clearly. avoid referencing IMGUI:s windowdata
// 		 members where possible
// wip: separate VK objects from generic ones.
// wip: dynamic mesh layout, depending on input data structure
// todo: instrumentation and timing information
// todo: move stuff from headers into compilation units
// todo: extract descriptor sets
// todo: resource loading / manager
// todo: graph based GUI
// todo: compute pipeline
// todo: clustered forward shading
// todo: frame graph / shader graph

#include "volcano.h"
#include "aabb.h"
#include "glm.h"
#include "file.h"
#include "gfx-types.h"
#include "math.h"
#include "utils.h"
#include "vertex.h"
#include "vk-utils.h"

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

#include <vulkan/vulkan.h>

#if defined(VOLCANO_USE_GLFW)
#	define GLFW_INCLUDE_NONE
#	include <GLFW/glfw3.h>
#endif

#if defined(__WINDOWS__)
#	include <wtypes.h>
#	include <vulkan/vulkan_win32.h>
#elif defined(__APPLE__)
#	include <vulkan/vulkan_macos.h>
#elif defined(__linux__)
#	if defined(VK_USE_PLATFORM_XCB_KHR)
#		include <X11/Xutil.h>
#		include <vulkan/vulkan_xcb.h>
#	elif defined(VK_USE_PLATFORM_XLIB_KHR)
#		include <X11/Xutil.h>
#		include <vulkan/vulkan_xlib.h>
#	elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
#		include <linux/input.h>
#		include <vulkan/vulkan_wayland.h>
#	endif
#endif

#define VMA_IMPLEMENTATION
#ifdef _DEBUG
#define VMA_DEBUG_INITIALIZE_ALLOCATIONS 1
#define VMA_DEBUG_MARGIN 16
#define VMA_DEBUG_DETECT_CORRUPTION 1
#endif
#include <vk_mem_alloc.h>

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

#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

#include <examples/imgui_impl_vulkan.cpp>
#include <imgui.cpp>
#include <imgui_demo.cpp>
#include <imgui_draw.cpp>
#include <imgui_widgets.cpp>

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
using ImGui_WindowData =
	std::conditional_t<B == GraphicsBackend::Vulkan, ImGui_ImplVulkanH_Window, std::nullptr_t>;

template <GraphicsBackend B>
struct ImGuiData
{
	ImGui_WindowData<B> window;
	std::vector<ImFont*> fonts;
};

template <GraphicsBackend B>
struct WindowData
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t framebufferWidth = 0;
	uint32_t framebufferHeight = 0;

	Surface<B> surface;
	SurfaceFormat<B> surfaceFormat;
	Format<B> depthFormat;
	PresentMode<B> presentMode;

	SwapchainContext<B> swapchain;

	std::vector<ViewData> views;
	std::optional<size_t> activeView;

	ImGuiData<B> imgui;
};

template <GraphicsBackend B>
struct GraphicsPipelineResourceContext // temp
{
	Buffer<B> uniformBuffer;
	Allocation<B> uniformBufferMemory;

	Model<B> model; // temp, use shared_ptrs instead

	Texture<B> texture; // temp, use shared_ptrs instead
	Sampler<B> sampler;

	WindowData<B>* window = nullptr; // temp - replace with generic render target structure
};

template <GraphicsBackend B>
struct PipelineConfiguration
{
	GraphicsPipelineResourceContext<B>* resources = nullptr; // todo: replace with shared_ptr
	PipelineLayoutContext<B>* layout = nullptr;				 // todo: replace with shared_ptr
	RenderPass<B> renderPass;
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

// this should be a temporary object only used during loading.
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

class VulkanApplication
{
public:
	VulkanApplication(
		void* view, int width, int height, int framebufferWidth, int framebufferHeight,
		const char* resourcePath, bool /*verbose*/)
		: myResourcePath(resourcePath)
		, myFrameCommandBufferThreadCount(4)
		, myRequestedCommandBufferThreadCount(myFrameCommandBufferThreadCount)
		, myGraphicsFrameTimestamp(std::chrono::high_resolution_clock::now())
	{
		assert(std::filesystem::is_directory(myResourcePath));

		myInstance = createInstance<GraphicsBackend::Vulkan>();
		myDebugCallback = createDebugCallback(myInstance);
		myWindow.surface = createSurface<GraphicsBackend::Vulkan>(myInstance, view);

		std::tie(
			myDevice, myPhysicalDevice, myWindow.swapchain.info, myWindow.surfaceFormat,
			myWindow.presentMode, myFrameCount, myQueue, myQueueFamilyIndex) =
			createDevice<GraphicsBackend::Vulkan>(myInstance, myWindow.surface);

		myAllocator = createAllocator(myDevice, myPhysicalDevice);

		myTransferCommandPool = createCommandPool(
			myDevice,
			VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
			myQueueFamilyIndex);

		createTextureSampler();
		createDescriptorPool();

		auto slangModule = loadSlangShaders<GraphicsBackend::Vulkan>("shaders.slang");

		auto createPipelineLayoutContext = [](VkDevice device, const auto& slangModule) {
			using VkPipelineLayoutContext = PipelineLayoutContext<GraphicsBackend::Vulkan>;
			VkPipelineLayoutContext pipelineLayout;

			using VkShaderModuleType = ShaderModule<GraphicsBackend::Vulkan>;
			auto vkShaderDeleter = [device](VkShaderModuleType* module, size_t size) {
				for (size_t i = 0; i < size; i++)
					vkDestroyShaderModule(device, *(module + i), nullptr);
			};
			pipelineLayout.shaders =
				std::unique_ptr<VkShaderModuleType[], ArrayDeleter<VkShaderModuleType>>(
					new VkShaderModuleType[slangModule.shaders.size()],
					{vkShaderDeleter, slangModule.shaders.size()});

			using VkDescriptorSetLayoutType = DescriptorSetLayout<GraphicsBackend::Vulkan>;
			auto vkDescriptorSetLayoutDeleter =
				[device](VkDescriptorSetLayoutType* layout, size_t size) {
					for (size_t i = 0; i < size; i++)
						vkDestroyDescriptorSetLayout(device, *(layout + i), nullptr);
				};
			pipelineLayout.descriptorSetLayouts = std::unique_ptr<
				VkDescriptorSetLayoutType[], ArrayDeleter<VkDescriptorSetLayoutType>>(
				new VkDescriptorSetLayoutType[slangModule.bindings.size()],
				{vkDescriptorSetLayoutDeleter, slangModule.bindings.size()});

			for (const auto& shader : slangModule.shaders)
			{
				auto createShaderModule = [](VkDevice device, const ShaderEntry& shader) {
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

		myPipelineLayout = createPipelineLayoutContext(myDevice, slangModule);

		myResources.model = loadModel("gallery.obj");
		myResources.texture = loadTexture("gallery.jpg");
		myResources.window = &myWindow;

		createBuffer(
			NX * NY * sizeof(UniformBufferObject), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, myResources.uniformBuffer,
			myResources.uniformBufferMemory, "uniformBuffer");

		createSwapchain(framebufferWidth, framebufferHeight);
		createFrameResources(
			width,
			height,
			framebufferWidth,
			framebufferHeight,
			myResources.model);

		float dpiScaleX = static_cast<float>(framebufferWidth) / width;
		float dpiScaleY = static_cast<float>(framebufferHeight) / height;

		initIMGUI(dpiScaleX, dpiScaleY);
	}

	~VulkanApplication()
	{
		CHECK_VK(vkDeviceWaitIdle(myDevice));

		cleanup();
	}

	void draw()
	{
		updateInput(myGraphicsDeltaTime.count());

		// re-create frame resources if needed
		if (myCreateFrameResourcesFlag)
		{
			CHECK_VK(vkQueueWaitIdle(myQueue));

			cleanupFrameResources();

			myFrameCommandBufferThreadCount = myRequestedCommandBufferThreadCount;

			createFrameResources(
				myWindow.width, myWindow.height, myWindow.framebufferWidth,
				myWindow.framebufferHeight, myResources.model);
		}

		// todo: run this at the same time as secondary command buffer recording
		updateUniformBuffers();

		// todo: run this at the same time as secondary command buffer recording
		if (myUIEnableFlag)
		{
			ImGui_ImplVulkan_NewFrame();
			ImGui::NewFrame();

			ImGui::ShowDemoWindow();

			{
				ImGui::Begin("Render Options");
				ImGui::DragInt(
					"Command Buffer Threads", &myRequestedCommandBufferThreadCount, 0.1f, 2, 32);
				ImGui::ColorEdit3(
					"Clear Color", &myWindow.imgui.window.ClearValue.color.float32[0]);
				ImGui::End();
			}

			{
				ImGui::Begin("GUI Options");
				// static int styleIndex = 0;
				ImGui::ShowStyleSelector("Styles" /*, &styleIndex*/);
				ImGui::ShowFontSelector("Fonts");
				if (ImGui::Button("Show User Guide"))
				{
					ImGui::SetNextWindowPosCenter(0);
					ImGui::OpenPopup("UserGuide");
				}
				if (ImGui::BeginPopup(
						"UserGuide", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
				{
					ImGui::ShowUserGuide();
					ImGui::EndPopup();
				}
				ImGui::End();
			}

			{
				ImGui::ShowMetricsWindow();
			}

			ImGui::Render();
		}

		submitFrame();
		presentFrame();
	}

	void resizeWindow(const window_state& state)
	{
		if (state.fullscreen_enabled)
		{
			myWindow.width = state.fullscreen_width;
			myWindow.height = state.fullscreen_height;
		}
		else
		{
			myWindow.width = state.width;
			myWindow.height = state.height;
		}
	}

	void resizeFramebuffer(int width, int height)
	{
		CHECK_VK(vkQueueWaitIdle(myQueue));

		cleanupFrameResources();

		// hack to shut up validation layer error.
		// see https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/624
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(myPhysicalDevice, myWindow.surface, &myWindow.swapchain.info.capabilities);

		createSwapchain(width, height, myWindow.swapchain.swapchain);
		createFrameResources(
			myWindow.width,
			myWindow.height,
			width,
			height,
			myResources.model);
	}

	void onMouse(const mouse_state& state)
	{
		bool leftPressed = false;
		bool rightPressed = false;
#if defined(VOLCANO_USE_GLFW)
		leftPressed = state.button == GLFW_MOUSE_BUTTON_LEFT && state.action == GLFW_PRESS;
		rightPressed = state.button == GLFW_MOUSE_BUTTON_RIGHT && state.action == GLFW_PRESS;
#endif

		auto screenPos = glm::vec2(state.xpos, state.ypos);
		// auto screenPos = ImGui::GetCursorScreenPos();

		if (state.inside_window && !myMouseButtonsPressed[0])
		{
			// todo: generic view index calculation
			size_t viewIdx = screenPos.x / (myWindow.width / NX);
			size_t viewIdy = screenPos.y / (myWindow.height / NY);
			myWindow.activeView = std::min((viewIdy * NX) + viewIdx, myWindow.views.size() - 1);

			// std::cout << *myWindow.activeView << ":[" << screenPos.x << ", " << screenPos.y << "]"
			// 		  << std::endl;
		}
		else if (!leftPressed)
		{
			myWindow.activeView.reset();
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
#if defined(VOLCANO_USE_GLFW)
		if (state.action == GLFW_PRESS)
			myKeysPressed[state.key] = true;
		else if (state.action == GLFW_RELEASE)
			myKeysPressed[state.key] = false;
#endif
	}

	void updateInput(float dt)
	{
		// update input dependent state
		{
			ImGuiIO& io = ImGui::GetIO();

			static bool escBufferState = false;
			bool escState = io.KeysDown[io.KeyMap[ImGuiKey_Escape]];
			if (escState && !escBufferState)
				myUIEnableFlag = !myUIEnableFlag;
			escBufferState = escState;
		}

		if (myFrameCommandBufferThreadCount != myRequestedCommandBufferThreadCount)
			myCreateFrameResourcesFlag = true;

		if (myWindow.activeView)
		{
			float dx = 0;
			float dz = 0;

#if defined(VOLCANO_USE_GLFW)
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
#endif

			auto& view = myWindow.views[*myWindow.activeView];

			bool doUpdateViewMatrix = false;

			if (dx != 0 || dz != 0)
			{
				auto forward = glm::vec3(view.view[0][2], view.view[1][2], view.view[2][2]);
				auto strafe = glm::vec3(view.view[0][0], view.view[1][0], view.view[2][0]);

				constexpr auto moveSpeed = 2.0f;

				view.camPos += dt * (dz * forward + dx * strafe) * moveSpeed;

				// std::cout << *myWindow.activeView << ":pos:[" << view.camPos.x << ", " <<
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

				// std::cout << *myWindow.activeView << ":rot:[" << view.camRot.x << ", " <<
				// view.camRot.y << ", " << view.camRot.z << "]" << std::endl;

				doUpdateViewMatrix = true;
			}

			if (doUpdateViewMatrix)
			{
				updateViewMatrix(*myWindow.activeView);
			}
		}
	}

private:
	Model<GraphicsBackend::Vulkan> loadModel(const char* filename) const
	{
		std::filesystem::path sourceFilePath(myResourcePath);
		sourceFilePath = std::filesystem::absolute(sourceFilePath);

		sourceFilePath /= "models";
		sourceFilePath /= filename;

		// todo: replace with generic structures / code?
		using VkInputAttributeDescription =
			SerializableVertexInputAttributeDescription<GraphicsBackend::Vulkan>;

		VertexAllocator vertices;
		std::vector<uint32_t> indices;
		std::vector<VkInputAttributeDescription> attributeDescriptions;
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
				VkInputAttributeDescription{{0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 0u}});
			attributeDescriptions.emplace_back(
				VkInputAttributeDescription{{1u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 12u}});
			attributeDescriptions.emplace_back(
				VkInputAttributeDescription{{2u, 0u, VK_FORMAT_R32G32_SFLOAT, 24u}});

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
			sourceFilePath, sourceFilePath, "tinyobjloader", "1.4.0", loadOBJ, loadPBin, savePBin);

		if (vertices.empty() || indices.empty())
			throw std::runtime_error("Failed to load model.");

		auto createModel = [this](
							   const auto& vertices, const std::vector<uint32_t>& indices,
							   const auto& attributeDescriptions, const char* filename) {
			using VkModel = Model<GraphicsBackend::Vulkan>;

			VkModel outModel;

			createDeviceLocalBuffer(
				vertices.data(), vertices.sizeBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				outModel.vertexBuffer, outModel.vertexBufferMemory, filename);

			createDeviceLocalBuffer(
				indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				outModel.indexBuffer, outModel.indexBufferMemory, filename);

			outModel.indexCount = indices.size();
			outModel.attributeDescriptions.reserve(attributeDescriptions.size());
			std::copy(
				attributeDescriptions.begin(), attributeDescriptions.end(),
				std::back_inserter(outModel.attributeDescriptions));
			outModel.bindingDescriptions = calculateInputBindingDescriptions(attributeDescriptions);

			return outModel;
		};

		return createModel(vertices, indices, attributeDescriptions, filename);
	}

	Texture<GraphicsBackend::Vulkan> loadTexture(const char* filename) const
	{
		std::filesystem::path imageFile(myResourcePath);
		imageFile = std::filesystem::absolute(imageFile);

		imageFile /= "images";
		imageFile /= filename;

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

		auto createTexture = [this](
								 const std::byte* data, int x, int y, VkFormat format,
								 VkImageUsageFlags flags, VkImageAspectFlagBits aspectFlags,
								 const char* debugName = nullptr) {
			Texture<GraphicsBackend::Vulkan> texture;

			createDeviceLocalImage2D(
				data, x, y, format, flags, texture.image, texture.imageMemory, debugName);

			texture.imageView = createImageView2D(texture.image, format, aspectFlags);

			return texture;
		};

		return createTexture(
			dxtImageData.get(), nx, ny,
			nChannels == 3 ? VK_FORMAT_BC1_RGB_UNORM_BLOCK
						   : VK_FORMAT_BC5_UNORM_BLOCK, // todo: write utility function for this
			VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, filename);
	}

	template <GraphicsBackend B>
	SerializableShaderReflectionModule<B> loadSlangShaders(const char* filename) const
	{
		std::filesystem::path slangFile(myResourcePath);
		slangFile = std::filesystem::absolute(slangFile);

		slangFile /= "shaders";
		slangFile /= filename;

		SerializableShaderReflectionModule<B> slangModule;

		auto loadPBin = [&slangModule](std::istream& stream) {
			cereal::PortableBinaryInputArchive pbin(stream);
			pbin(slangModule);
		};

		auto savePBin = [&slangModule](std::ostream& stream) {
			cereal::PortableBinaryOutputArchive pbin(stream);
			pbin(slangModule);
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

				slangModule.shaders.emplace_back(std::make_pair(blob->getBufferSize(), ep));
				std::copy(
					static_cast<const std::byte*>(blob->getBufferPointer()),
					static_cast<const std::byte*>(blob->getBufferPointer()) + blob->getBufferSize(),
					slangModule.shaders.back().first.data());
				blob->release();
			}

			auto& bindings = slangModule.bindings;

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

		if (slangModule.shaders.empty())
			throw std::runtime_error("Failed to load shaders.");

		return slangModule;
	}

	template <GraphicsBackend B>
	Instance<B> createInstance() const
	{
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

		const char* enabledLayerNames[] = {"VK_LAYER_LUNARG_standard_validation"};

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
		info.enabledLayerCount = 1;
		info.ppEnabledLayerNames = enabledLayerNames;
		info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
		info.ppEnabledExtensionNames = requiredExtensions.data();

		VkInstance instance = VK_NULL_HANDLE;
		CHECK_VK(vkCreateInstance(&info, NULL, &instance));

		return instance;
	}

	template <GraphicsBackend B>
	Surface<B> createSurface(Instance<B> instance, void* view) const
	{
		Surface<B> surface;
#if defined(VOLCANO_USE_GLFW)
		CHECK_VK(glfwCreateWindowSurface(
			instance, reinterpret_cast<GLFWwindow*>(view), nullptr, &surface));
#elif defined(__WINDOWS__)
		VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.hwnd = (HWND)view;
		surfaceCreateInfo.hinstance = GetModuleHandle(nullptr);
		auto vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(
			myInstance, "vkCreateWin32SurfaceKHR");
		assert(vkCreateWin32SurfaceKHR != nullptr);
		CHECK_VK(vkCreateWin32SurfaceKHR(instance, &surfaceCreateInfo, nullptr, &surface));
#elif defined(__APPLE__)
	VkMacOSSurfaceCreateInfoMVK surfaceCreateInfo = {};
	surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK;
	surfaceCreateInfo.flags = 0;
	surfaceCreateInfo.pView = view;
	auto vkCreateMacOSSurfaceMVK =
		(PFN_vkCreateMacOSSurfaceMVK)vkGetInstanceProcAddr(instance, "vkCreateMacOSSurfaceMVK");
	assert(vkCreateMacOSSurfaceMVK != nullptr);
	CHECK_VK(vkCreateMacOSSurfaceMVK(instance, &surfaceCreateInfo, nullptr, &surface));
#elif defined(__linux__)
#	if defined(VK_USE_PLATFORM_XCB_KHR)
	VkXcbSurfaceCreateInfoKHR surfaceCreateInfo = {};
	surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
	surfaceCreateInfo.flags = 0;
	surfaceCreateInfo.connection = nullptr; //?;
	surfaceCreateInfo.window = nullptr;		//?;
	auto vkCreateXcbSurfaceKHR =
		(PFN_vkCreateXcbSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR");
	assert(vkCreateXcbSurfaceKHR != nullptr);
	CHECK_VK(vkCreateXcbSurfaceKHR(instance, &surfaceCreateInfo, nullptr, &surface));
#	elif defined(VK_USE_PLATFORM_XLIB_KHR)
	VkXlibSurfaceCreateInfoKHR surfaceCreateInfo = {};
	surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
	surfaceCreateInfo.flags = 0;
	surfaceCreateInfo.dpy = nullptr;	//?;
	surfaceCreateInfo.window = nullptr; //?;
	auto vkCreateXlibSurfaceKHR =
		(PFN_vkCreateXlibSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR");
	assert(vkCreateXlibSurfaceKHR != nullptr);
	CHECK_VK(vkCreateXlibSurfaceKHR(instance, &surfaceCreateInfo, nullptr, &surface));
#	elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
	VkWaylandSurfaceCreateInfoKHR surfaceCreateInfo = {};
	surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	surfaceCreateInfo.flags = 0;
	surfaceCreateInfo.wl_display = nullptr; //?;
	surfaceCreateInfo.wl_surface = nullptr; //?;
	auto vkCreateWaylandSurfaceKHR =
		(PFN_vkCreateWaylandSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR");
	assert(vkCreateWaylandSurfaceKHR != nullptr);
	CHECK_VK(vkCreateWaylandSurfaceKHR(instance, &surfaceCreateInfo, nullptr, &surface));
#	endif
#endif
		return surface;
	}

	template <GraphicsBackend B>
	std::tuple<
		Device<B>, PhysicalDevice<B>, SwapchainInfo<B>, SurfaceFormat<B>, PresentMode<B>, uint32_t,
		Queue<B>, int>
	createDevice(Instance<B> instance, Surface<B> surface) const
	{
		Device<B> logicalDevice;
		PhysicalDevice<B> physicalDevice;
		SwapchainInfo<B> swapChainInfo;
		SurfaceFormat<B> selectedSurfaceFormat;
		PresentMode<B> selectedPresentMode;
		uint32_t selectedFrameCount;
		Queue<B> selectedQueue;
		int selectedQueueFamilyIndex = -1;

		uint32_t deviceCount = 0;
		CHECK_VK(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
		if (deviceCount == 0)
			throw std::runtime_error("failed to find GPUs with Vulkan support!");

		std::vector<VkPhysicalDevice> devices(deviceCount);
		CHECK_VK(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()));

		for (const auto& device : devices)
		{
			std::tie(swapChainInfo, selectedQueueFamilyIndex) =
				getSuitableSwapchainAndQueueFamilyIndex(surface, device);

			if (selectedQueueFamilyIndex >= 0)
			{
				physicalDevice = device;

				const VkFormat requestSurfaceImageFormat[] = {
					VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM,
					VK_FORMAT_R8G8B8_UNORM};
				const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
				const VkPresentModeKHR requestPresentMode[] = {
					VK_PRESENT_MODE_MAILBOX_KHR,
					VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR,
					VK_PRESENT_MODE_IMMEDIATE_KHR};

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

						if (myWindow.presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
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
			logicalDevice, physicalDevice, swapChainInfo, selectedSurfaceFormat,
			selectedPresentMode, selectedFrameCount, selectedQueue, selectedQueueFamilyIndex);
	}

	void createSwapchain(int width, int height, VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE)
	{
		{
			VkSwapchainCreateInfoKHR info = {};
			info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
			info.surface = myWindow.surface;
			info.minImageCount = myFrameCount;
			info.imageFormat = myWindow.surfaceFormat.format;
			info.imageColorSpace = myWindow.surfaceFormat.colorSpace;
			info.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
			info.imageArrayLayers = 1;
			info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			info.imageSharingMode =
				VK_SHARING_MODE_EXCLUSIVE; // Assume that graphics family == present family
			info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
			info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			info.presentMode = myWindow.presentMode;
			info.clipped = VK_TRUE;
			info.oldSwapchain = oldSwapchain;

			CHECK_VK(vkCreateSwapchainKHR(myDevice, &info, nullptr, &myWindow.swapchain.swapchain));
		}

		if (oldSwapchain)
		{
			vkDestroySwapchainKHR(myDevice, oldSwapchain, nullptr);
			vmaDestroyImage(myAllocator, myWindow.swapchain.depthImage, myWindow.swapchain.depthImageMemory);
		}

		uint32_t imageCount;
		CHECK_VK(
			vkGetSwapchainImagesKHR(myDevice, myWindow.swapchain.swapchain, &imageCount, nullptr));
		myWindow.swapchain.frameBuffers.resize(imageCount);
		myWindow.swapchain.colorImages.resize(imageCount);
		myWindow.swapchain.colorImageViews.resize(imageCount);
		CHECK_VK(vkGetSwapchainImagesKHR(
			myDevice, myWindow.swapchain.swapchain, &imageCount,
			myWindow.swapchain.colorImages.data()));

		myWindow.depthFormat = findSupportedFormat(
			myPhysicalDevice,
			{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
			VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);

		createImage2D(
			width, height, myWindow.depthFormat, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			myWindow.swapchain.depthImage, myWindow.swapchain.depthImageMemory, "depthImage");

		transitionImageLayout(
			myWindow.swapchain.depthImage, myWindow.depthFormat, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}

	VmaAllocator createAllocator(VkDevice device, VkPhysicalDevice physicalDevice) const
	{
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

	void createDescriptorPool()
	{
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

		CHECK_VK(vkCreateDescriptorPool(myDevice, &poolInfo, nullptr, &myDescriptorPool));
	}

	void createGraphicsRenderPass()
	{
		VkAttachmentDescription colorAttachment = {};
		colorAttachment.format = myWindow.surfaceFormat.format;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentDescription depthAttachment = {};
		depthAttachment.format = myWindow.depthFormat;
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

		CHECK_VK(
			vkCreateRenderPass(myDevice, &renderPassInfo, nullptr, &myPipelineConfig.renderPass));
	}

	void createPipelineConfig(const Model<GraphicsBackend::Vulkan>& model)
	{
		auto createVkGraphicsPipeline =
			[](VkDevice device, const PipelineConfiguration<GraphicsBackend::Vulkan>& pipeline,
			   const Model<GraphicsBackend::Vulkan>& model) {
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
				vertexInputInfo.vertexBindingDescriptionCount = model.bindingDescriptions.size();
				vertexInputInfo.pVertexBindingDescriptions = model.bindingDescriptions.data();
				vertexInputInfo.vertexAttributeDescriptionCount =
					static_cast<uint32_t>(model.attributeDescriptions.size());
				vertexInputInfo.pVertexAttributeDescriptions = model.attributeDescriptions.data();

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

				VkPipeline outPipeline;

				CHECK_VK(vkCreateGraphicsPipelines(
					device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline));

				return outPipeline;
			};

		myPipelineConfig.resources = &myResources;
		myPipelineConfig.layout = &myPipelineLayout;

		myGraphicsPipeline = createVkGraphicsPipeline(myDevice, myPipelineConfig, model);
		myDescriptorSets = allocateDescriptorSets(
			myDevice, myDescriptorPool, myPipelineLayout.descriptorSetLayouts.get(),
			myPipelineLayout.descriptorSetLayouts.get_deleter().size);
	}

	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) const
	{
		VkCommandBuffer commandBuffer = beginSingleTimeCommands(myDevice, myTransferCommandPool);

		VkBufferCopy copyRegion = {};
		copyRegion.srcOffset = 0;
		copyRegion.dstOffset = 0;
		copyRegion.size = size;
		vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

		endSingleTimeCommands(myDevice, myQueue, commandBuffer, myTransferCommandPool);
	}

	void createBuffer(
		VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
		VkBuffer& outBuffer, VmaAllocation& outBufferMemory, const char* debugName) const
	{
		VkBufferCreateInfo bufferInfo = {};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
		allocInfo.usage = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? VMA_MEMORY_USAGE_GPU_ONLY
																		: VMA_MEMORY_USAGE_UNKNOWN;
		allocInfo.requiredFlags = flags;
		allocInfo.memoryTypeBits = 0; // memRequirements.memoryTypeBits;
		allocInfo.pUserData = (void*)debugName;

		CHECK_VK(vmaCreateBuffer(
			myAllocator, &bufferInfo, &allocInfo, &outBuffer, &outBufferMemory, nullptr));
	}

	template <typename T>
	void createDeviceLocalBuffer(
		const T* bufferData, size_t bufferSize, VkBufferUsageFlags usage, VkBuffer& outBuffer,
		VmaAllocation& outBufferMemory, const char* debugName) const
	{
		assert(bufferData != nullptr);
		assert(bufferSize > 0);

		// todo: use staging buffer pool, or use scratchpad memory
		VkBuffer stagingBuffer;
		VmaAllocation stagingBufferMemory;
		std::string debugString;
		debugString.append(debugName);
		debugString.append("_staging");
		createBuffer(
			bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer, stagingBufferMemory, debugString.c_str());

		void* data;
		CHECK_VK(vmaMapMemory(myAllocator, stagingBufferMemory, &data));
		memcpy(data, bufferData, bufferSize);
		vmaUnmapMemory(myAllocator, stagingBufferMemory);

		createBuffer(
			bufferSize, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outBuffer, outBufferMemory, debugName);

		copyBuffer(stagingBuffer, outBuffer, bufferSize);

		vmaDestroyBuffer(myAllocator, stagingBuffer, stagingBufferMemory);
	}

	void transitionImageLayout(
		VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) const
	{
		VkCommandBuffer commandBuffer = beginSingleTimeCommands(myDevice, myTransferCommandPool);

		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;

		if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
		{
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

			if (hasStencilComponent(format))
				barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
		else
		{
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		}

		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		VkPipelineStageFlags sourceStage = 0;
		VkPipelineStageFlags destinationStage = 0;

		if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
			newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		{
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if (
			oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
			newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		{
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			destinationStage =
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; // wrong of accessed by another shader type
		}
		else if (
			oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
			newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
		{
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
									VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

			sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		}
		else
		{
			assert(false); // not implemented yet
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = 0;
		}

		vkCmdPipelineBarrier(
			commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		endSingleTimeCommands(myDevice, myQueue, commandBuffer, myTransferCommandPool);
	}

	void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) const
	{
		VkCommandBuffer commandBuffer = beginSingleTimeCommands(myDevice, myTransferCommandPool);

		VkBufferImageCopy region = {};
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = {0, 0, 0};
		region.imageExtent = {width, height, 1};

		vkCmdCopyBufferToImage(
			commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		endSingleTimeCommands(myDevice, myQueue, commandBuffer, myTransferCommandPool);
	}

	void createImage2D(
		uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling,
		VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags, VkImage& outImage,
		VmaAllocation& outImageMemory, const char* debugName) const
	{
		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = width;
		imageInfo.extent.height = height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = format;
		imageInfo.tiling = tiling;
		imageInfo.usage = usage;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.flags = 0;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
		allocInfo.usage = (memoryFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
							  ? VMA_MEMORY_USAGE_GPU_ONLY
							  : VMA_MEMORY_USAGE_UNKNOWN;
		allocInfo.requiredFlags = memoryFlags;
		allocInfo.memoryTypeBits = 0; // memRequirements.memoryTypeBits;
		allocInfo.pUserData = (void*)debugName;

		VmaAllocationInfo outAllocInfo = {};
		CHECK_VK(vmaCreateImage(
			myAllocator, &imageInfo, &allocInfo, &outImage, &outImageMemory, &outAllocInfo));
	}

	template <typename T>
	void createDeviceLocalImage2D(
		const T* imageData, uint32_t width, uint32_t height, VkFormat format,
		VkImageUsageFlags usage, VkImage& outImage, VmaAllocation& outImageMemory,
		const char* debugName) const
	{
		assert((width & 1) == 0);
		assert((height & 1) == 0);

		uint32_t pixelSizeBytesDivisor;
		uint32_t pixelSizeBytes = getFormatSize(format, pixelSizeBytesDivisor);
		VkDeviceSize imageSize = width * height * pixelSizeBytes / pixelSizeBytesDivisor;

		VkBuffer stagingBuffer;
		VmaAllocation stagingBufferMemory;
		std::string debugString;
		debugString.append(debugName);
		debugString.append("_staging");
		createBuffer(
			imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer, stagingBufferMemory, debugString.c_str());

		void* data;
		CHECK_VK(vmaMapMemory(myAllocator, stagingBufferMemory, &data));
		memcpy(data, imageData, imageSize);
		vmaUnmapMemory(myAllocator, stagingBufferMemory);

		createImage2D(
			width, height, format, VK_IMAGE_TILING_OPTIMAL, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outImage, outImageMemory, debugName);

		transitionImageLayout(
			outImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		copyBufferToImage(stagingBuffer, outImage, width, height);
		transitionImageLayout(
			outImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(myAllocator, stagingBuffer, stagingBufferMemory);
	}

	VkImageView
	createImageView2D(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) const
	{
		VkImageView imageView;
		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = aspectFlags;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;
		viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

		CHECK_VK(vkCreateImageView(myDevice, &viewInfo, nullptr, &imageView));

		return imageView;
	}

	void createTextureSampler()
	{
		VkSamplerCreateInfo samplerInfo = {};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.anisotropyEnable = VK_TRUE;
		samplerInfo.maxAnisotropy = 16;
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.mipLodBias = 0.0f;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 0.0f;

		CHECK_VK(vkCreateSampler(myDevice, &samplerInfo, nullptr, &myResources.sampler));
	}

	void initIMGUI(float dpiScaleX, float dpiScaleY)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
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

		for (auto font : fonts)
		{
			fontPath.replace_filename(font);
			myWindow.imgui.fonts.emplace_back(io.Fonts->AddFontFromFileTTF(
				fontPath.string().c_str(), 16.0f,
				&config) /*,[](ImFont* f) { ImGui::MemFree(f); }*/);
		}

		// Setup style
		ImGui::StyleColorsClassic();
		io.FontDefault = myWindow.imgui.fonts.back();

		// Setup Vulkan binding
		ImGui_ImplVulkan_InitInfo initInfo = {};
		initInfo.Instance = myInstance;
		initInfo.PhysicalDevice = myPhysicalDevice;
		initInfo.Device = myDevice;
		initInfo.QueueFamily = myQueueFamilyIndex;
		initInfo.Queue = myQueue;
		initInfo.PipelineCache = VK_NULL_HANDLE;
		initInfo.DescriptorPool = myDescriptorPool;
		initInfo.MinImageCount = myFrameCount;
    	initInfo.ImageCount = myFrameCount;
		initInfo.Allocator = nullptr; // myAllocator;
		// initInfo.HostAllocationCallbacks = nullptr;
		initInfo.CheckVkResultFn = CHECK_VK;
		ImGui_ImplVulkan_Init(&initInfo, myPipelineConfig.renderPass);

		// Upload Fonts
		{
			VkCommandBuffer commandBuffer =
				beginSingleTimeCommands(myDevice, myTransferCommandPool);
			ImGui_ImplVulkan_CreateFontsTexture(commandBuffer);
			endSingleTimeCommands(myDevice, myQueue, commandBuffer, myTransferCommandPool);
			ImGui_ImplVulkan_DestroyFontUploadObjects();
		}
	}

	void createFrameResources(
		int width, int height, int framebufferWidth, int framebufferHeight,
		const Model<GraphicsBackend::Vulkan>& model)
	{
		myResources.window = &myWindow;

		myWindow.width = width;
		myWindow.height = height;
		myWindow.framebufferWidth = framebufferWidth;
		myWindow.framebufferHeight = framebufferHeight;

		myWindow.views.resize(NX * NY);
		for (auto& view : myWindow.views)
		{
			if (!view.viewport.width)
				view.viewport.width = framebufferWidth / NX;

			if (!view.viewport.height)
				view.viewport.height = framebufferHeight / NY;

			size_t viewIndex = &view - &myWindow.views[0];

			updateViewMatrix(viewIndex);
			updateProjectionMatrix(viewIndex);
		}

		createGraphicsRenderPass();

		auto createSwapchainImageViews = [this]()
		{
			for (uint32_t i = 0; i < myWindow.swapchain.colorImages.size(); i++)
			myWindow.swapchain.colorImageViews[i] = createImageView2D(
				myWindow.swapchain.colorImages[i], myWindow.surfaceFormat.format,
				VK_IMAGE_ASPECT_COLOR_BIT);

			myWindow.swapchain.depthImageView = createImageView2D(
				myWindow.swapchain.depthImage, myWindow.depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
		};

		createSwapchainImageViews();

		auto createFramebuffers = [this](uint32_t width, uint32_t height) {
			std::array<VkImageView, 2> attachments = {nullptr, myWindow.swapchain.depthImageView};
			VkFramebufferCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			info.renderPass = myPipelineConfig.renderPass;
			info.attachmentCount = attachments.size();
			info.pAttachments = attachments.data();
			info.width = width;
			info.height = height;
			info.layers = 1;
			for (uint32_t i = 0; i < myWindow.swapchain.frameBuffers.size(); i++)
			{
				attachments[0] = myWindow.swapchain.colorImageViews[i];
				CHECK_VK(vkCreateFramebuffer(
					myDevice, &info, nullptr, &myWindow.swapchain.frameBuffers[i]));
			}
		};

		createFramebuffers(framebufferWidth, framebufferHeight);
		
		createPipelineConfig(model);

		auto& window = myWindow.imgui.window;
		window.ImageCount = myFrameCount;

		// vkAcquireNextImageKHR uses semaphore from last frame -> cant use index 0 for first frame
		window.FrameIndex = window.ImageCount - 1;

		window.Width = framebufferWidth;
		window.Height = framebufferHeight;

		window.Swapchain = myWindow.swapchain.swapchain;
		window.SurfaceFormat = myWindow.surfaceFormat;
		window.Surface = myWindow.surface;
		window.PresentMode = myWindow.presentMode;

		window.RenderPass = myPipelineConfig.renderPass;

		// we will clear before IMGUI, using window.ClearValue
		window.ClearEnable = false;
		window.ClearValue.color.float32[0] = 0.4f;
		window.ClearValue.color.float32[1] = 0.4f;
		window.ClearValue.color.float32[2] = 0.5f;
		window.ClearValue.color.float32[3] = 1.0f;

		window.Frames = (ImGui_ImplVulkanH_Frame*)malloc(sizeof(ImGui_ImplVulkanH_Frame) * window.ImageCount);
        window.FrameSemaphores = (ImGui_ImplVulkanH_FrameSemaphores*)malloc(sizeof(ImGui_ImplVulkanH_FrameSemaphores) * window.ImageCount);

		for (uint32_t imageIt = 0; imageIt < myWindow.swapchain.colorImages.size(); imageIt++)
		{
			window.Frames[imageIt].Backbuffer = myWindow.swapchain.colorImages[imageIt];
			window.Frames[imageIt].BackbufferView =
				myWindow.swapchain.colorImageViews[imageIt];
			window.Frames[imageIt].Framebuffer = myWindow.swapchain.frameBuffers[imageIt];
		}

		myFrameCommandPools.resize(myFrameCommandBufferThreadCount);
		myFrameCommandBuffers.resize(myFrameCommandBufferThreadCount * myFrameCount);

		std::vector<VkCommandBuffer> threadCommandBuffers;
		for (uint32_t cmdIt = 0; cmdIt < myFrameCommandBufferThreadCount; cmdIt++)
		{
			myFrameCommandPools[cmdIt] = createCommandPool(
				myDevice,
				VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
					VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
				myQueueFamilyIndex);

			threadCommandBuffers = allocateCommandBuffers(
				myDevice, myFrameCommandPools[cmdIt],
				cmdIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY,
				myFrameCount);

			for (uint32_t frameIt = 0; frameIt < myFrameCount; frameIt++)
				myFrameCommandBuffers[myFrameCommandBufferThreadCount * frameIt + cmdIt] =
					threadCommandBuffers[frameIt];
		}

		myFrameFences.resize(myFrameCount);
		myImageAcquiredSemaphores.resize(myFrameCount);
		myRenderCompleteSemaphores.resize(myFrameCount);

		for (uint32_t frameIt = 0; frameIt < myFrameCount; frameIt++)
		{
			VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			CHECK_VK(vkCreateFence(myDevice, &fenceInfo, nullptr, &myFrameFences[frameIt]));

			VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			CHECK_VK(vkCreateSemaphore(
				myDevice, &semaphoreInfo, nullptr, &myImageAcquiredSemaphores[frameIt]));
			CHECK_VK(vkCreateSemaphore(
				myDevice, &semaphoreInfo, nullptr, &myRenderCompleteSemaphores[frameIt]));

			// IMGUI uses primary command buffer only
			auto& fd = window.Frames[frameIt];
			fd.CommandPool = myFrameCommandPools[0];
			fd.CommandBuffer = myFrameCommandBuffers[myFrameCommandBufferThreadCount * frameIt];
			fd.Fence = myFrameFences[frameIt];

			auto& fs = window.FrameSemaphores[frameIt];
			fs.ImageAcquiredSemaphore = myImageAcquiredSemaphores[frameIt];
			fs.RenderCompleteSemaphore = myRenderCompleteSemaphores[frameIt];
		}

		auto updateDescriptorSets = [this]()
		{
			VkDescriptorBufferInfo bufferInfo = {};
			bufferInfo.buffer = myResources.uniformBuffer;
			bufferInfo.offset = 0;
			bufferInfo.range = VK_WHOLE_SIZE;

			VkDescriptorImageInfo imageInfo = {};
			imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo.imageView = myResources.texture.imageView;
			imageInfo.sampler = myResources.sampler;

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
				myDevice, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0,
				nullptr);
		};

		updateDescriptorSets();
	}

	void checkFlipOrPresentResult(VkResult result)
	{
		switch (result)
		{
		case VK_SUCCESS:
			break;
		case VK_SUBOPTIMAL_KHR:
			std::cout << "warning: flip/present returned VK_SUBOPTIMAL_KHR";
			break;
		case VK_ERROR_OUT_OF_DATE_KHR:
			myCreateFrameResourcesFlag = true;
			break;
		default:
			throw std::runtime_error("failed to flip swap chain image!");
		}
	}

	void updateViewMatrix(size_t index)
	{
		auto& view = myWindow.views[index];

		auto Rx = glm::rotate(glm::mat4(1.0), view.camRot.x, glm::vec3(-1, 0, 0));
		auto Ry = glm::rotate(glm::mat4(1.0), view.camRot.y, glm::vec3(0, -1, 0));
		auto T = glm::translate(glm::mat4(1.0), -view.camPos);

		view.view = glm::inverse(T * Ry * Rx);
	}

	void updateProjectionMatrix(size_t index)
	{
		auto& view = myWindow.views[index];

		constexpr glm::mat4 clip = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
									0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f,  0.5f, 1.0f};

		constexpr auto fov = 75.0f;
		auto aspect =
			static_cast<float>(view.viewport.width) / static_cast<float>(view.viewport.height);
		constexpr auto nearplane = 0.01f;
		constexpr auto farplane = 100.0f;

		view.projection = clip * glm::perspective(glm::radians(fov), aspect, nearplane, farplane);
	}

	void updateUniformBuffers()
	{
		UniformBufferObject* data;
		CHECK_VK(vmaMapMemory(myAllocator, myResources.uniformBufferMemory, (void**)&data));

		// static auto start = std::chrono::high_resolution_clock::now();
		// auto now = std::chrono::high_resolution_clock::now();
		// constexpr float period = 10.0;
		// float t = std::chrono::duration<float>(now - start).count();

		for (uint32_t n = 0; n < (NX * NY); n++)
		{
			UniformBufferObject& ubo = data[n];

			// float tp = fmod((0.0025f * n) + t, period);
			// float s = smootherstep(
			// 	smoothstep(clamp(ramp(tp < (0.5f * period) ? tp : period - tp, 0, 0.5f * period), 0,
			// 1)));

			// const auto& model = glm::rotate(
			// 	glm::translate(
			// 		myResources.model.transform,
			// 		glm::vec3(0, 0, -0.01f - std::numeric_limits<float>::epsilon())),
			// 	s * glm::radians(360.0f),
			//  	glm::vec3(0.0, 0.0, 1.0));

			// glm::mat4 view0 = glm::mat4(1);
			// glm::mat4 proj0 = glm::frustum(-1.0, 1.0, -1.0, 1.0, 0.01, 10.0);
			// glm::mat4 view1 =
			// 	glm::lookAt(
			// 		glm::vec3(1.5f, 1.5f, 1.0f),
			// 		glm::vec3(0.0f, 0.0f, -0.5f),
			// 		glm::vec3(0.0f, 0.0f, -1.0f));
			// glm::mat4 proj1 =
			// 	glm::perspective(
			// 		glm::radians(75.0f),
			// 		myWindow.swapchain.width / static_cast<float>(myWindow.swapchain.height),
			// 		0.01f,
			// 		10.0f);

			// const auto& view = glm::mat4(
			// 	lerp(view0[0], view1[0], s),
			// 	lerp(view0[1], view1[1], s),
			// 	lerp(view0[2], view1[2], s),
			// 	lerp(view0[3], view1[3], s));
			// const auto& proj = glm::mat4(
			// 	lerp(proj0[0], proj1[0], s),
			// 	lerp(proj0[1], proj1[1], s),
			// 	lerp(proj0[2], proj1[2], s),
			// 	lerp(proj0[3], proj1[3], s));

			ubo.model = glm::mat4(1.0f); // myResources.model.transform;
			ubo.view = glm::mat4(myWindow.views[n].view);
			ubo.proj = myWindow.views[n].projection;
		}

		vmaFlushAllocation(myAllocator, myResources.uniformBufferMemory, 0, VK_WHOLE_SIZE);
		vmaUnmapMemory(myAllocator, myResources.uniformBufferMemory);
	}

	void submitFrame()
	{
		auto& window = myWindow.imgui.window;
		VkSemaphore& imageAquiredSemaphore = 
			window.FrameSemaphores[window.FrameIndex].ImageAcquiredSemaphore;

		checkFlipOrPresentResult(vkAcquireNextImageKHR(
			myDevice, window.Swapchain, UINT64_MAX, imageAquiredSemaphore,
			VK_NULL_HANDLE, &window.FrameIndex));

		/* TODO: MGPU method from vk 1.1 spec
		{
			VkAcquireNextImageInfoKHR nextImageInfo = {};
			nextImageInfo.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR;
			nextImageInfo.swapchain = window.Swapchain;
			nextImageInfo.timeout = UINT64_MAX;
			nextImageInfo.semaphore = imageAquiredSemaphore;
			nextImageInfo.fence = VK_NULL_HANDLE;
			nextImageInfo.deviceMask = ?;

			checkFlipOrPresentResult(vkAcquireNextImage2KHR(myDevice, &nextImageInfo,
		&window.FrameIndex));
		}
		 */

		auto& frame = window.Frames[window.FrameIndex];
		auto& frameSemaphores = window.FrameSemaphores[window.FrameIndex];

		std::array<VkClearValue, 2> clearValues = {};
		clearValues[0] = window.ClearValue;
		clearValues[1].depthStencil = {1.0f, 0};

		// wait for frame to be completed before starting to use it
		{
			CHECK_VK(vkWaitForFences(myDevice, 1, &frame.Fence, VK_TRUE, UINT64_MAX));
			CHECK_VK(vkResetFences(myDevice, 1, &frame.Fence));

			auto lastGraphicsFrameTimestamp = myGraphicsFrameTimestamp;
			myGraphicsFrameTimestamp = std::chrono::high_resolution_clock::now();
			myGraphicsDeltaTime = myGraphicsFrameTimestamp - lastGraphicsFrameTimestamp;
		}

		// setup draw parameters
		constexpr uint32_t drawCount = NX * NY;
		uint32_t segmentCount = std::max(myFrameCommandBufferThreadCount - 1u, 1u);

		assert(myPipelineConfig.resources != nullptr);
		auto& resources = *myPipelineConfig.resources;

		// begin secondary command buffers
		for (uint32_t segmentIt = 0; segmentIt < segmentCount; segmentIt++)
		{
			VkCommandBuffer& cmd = myFrameCommandBuffers
				[window.FrameIndex * myFrameCommandBufferThreadCount +
				 (segmentIt + 1)];

			VkCommandBufferInheritanceInfo inherit = {
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
			inherit.renderPass = myPipelineConfig.renderPass;
			inherit.framebuffer =
				window.Frames[window.FrameIndex].Framebuffer;

			CHECK_VK(vkResetCommandBuffer(cmd, 0));
			VkCommandBufferBeginInfo secBeginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			secBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
								 VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
								 VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
			secBeginInfo.pInheritanceInfo = &inherit;
			CHECK_VK(vkBeginCommandBuffer(cmd, &secBeginInfo));

			// bind pipeline and vertex/index buffers
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, myGraphicsPipeline);

			VkBuffer vertexBuffers[] = {resources.model.vertexBuffer};
			VkDeviceSize vertexOffsets[] = {0};

			vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
			vkCmdBindIndexBuffer(cmd, resources.model.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
		}

		// draw geometry using secondary command buffers
		{
			uint32_t segmentDrawCount = drawCount / segmentCount;
			if (drawCount % segmentCount)
				segmentDrawCount += 1;

			uint32_t dx = myWindow.framebufferWidth / NX;
			uint32_t dy = myWindow.framebufferHeight / NY;

			uint32_t frameIndex = window.FrameIndex;

			std::array<uint32_t, 128> seq;
			std::iota(seq.begin(), seq.begin() + segmentCount, 0);
			std::for_each_n(
#if defined(__WINDOWS__)
				std::execution::par,
#endif
				seq.begin(), segmentCount, [this, &dx, &dy, &segmentDrawCount, &frameIndex](uint32_t segmentIt) {
					VkCommandBuffer& cmd = myFrameCommandBuffers
						[frameIndex * myFrameCommandBufferThreadCount +
						 (segmentIt + 1)];

					for (uint32_t drawIt = 0; drawIt < segmentDrawCount; drawIt++)
					{
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

						auto drawModel = [&n](
											 VkCommandBuffer cmd, uint32_t indexCount,
											 uint32_t descriptorSetCount,
											 const VkDescriptorSet* descriptorSets,
											 VkPipelineLayout pipelineLayout) {
							uint32_t uniformBufferOffset = n * sizeof(UniformBufferObject);
							vkCmdBindDescriptorSets(
								cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
								descriptorSetCount, descriptorSets, 1, &uniformBufferOffset);
							vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
						};

						setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

						drawModel(
							cmd, myResources.model.indexCount, myDescriptorSets.size(),
							myDescriptorSets.data(), myPipelineLayout.layout);
					}
				});
		}

		// end secondary command buffers
		for (uint32_t segmentIt = 0; segmentIt < segmentCount; segmentIt++)
		{
			VkCommandBuffer& cmd = myFrameCommandBuffers
				[window.FrameIndex * myFrameCommandBufferThreadCount +
				 (segmentIt + 1)];

			CHECK_VK(vkEndCommandBuffer(cmd));
		}

		// begin primary command buffer
		{
			CHECK_VK(vkResetCommandBuffer(frame.CommandBuffer, 0));
			VkCommandBufferBeginInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			CHECK_VK(vkBeginCommandBuffer(frame.CommandBuffer, &info));
		}

		// call secondary command buffers
		{
			VkRenderPassBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			beginInfo.renderPass = myPipelineConfig.renderPass;
			beginInfo.framebuffer = frame.Framebuffer;
			beginInfo.renderArea.offset = {0, 0};
			beginInfo.renderArea.extent = {static_cast<uint32_t>(myWindow.framebufferWidth),
										   static_cast<uint32_t>(myWindow.framebufferHeight)};
			beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
			beginInfo.pClearValues = clearValues.data();
			vkCmdBeginRenderPass(
				frame.CommandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

			vkCmdExecuteCommands(
				frame.CommandBuffer, (myFrameCommandBufferThreadCount - 1),
				&myFrameCommandBuffers
					[(window.FrameIndex * myFrameCommandBufferThreadCount) + 1]);

			vkCmdEndRenderPass(frame.CommandBuffer);
		}

		// Record Imgui Draw Data and draw funcs into primary command buffer
		if (myUIEnableFlag)
		{
			VkRenderPassBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			beginInfo.renderPass = window.RenderPass;
			beginInfo.framebuffer = frame.Framebuffer;
			beginInfo.renderArea.extent.width = window.Width;
			beginInfo.renderArea.extent.height = window.Height;
			beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
			beginInfo.pClearValues = clearValues.data();
			vkCmdBeginRenderPass(frame.CommandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.CommandBuffer);

			vkCmdEndRenderPass(frame.CommandBuffer);
		}

		// Submit primary command buffer
		{
			VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			VkSubmitInfo submitInfo = {};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.waitSemaphoreCount = 1;
			submitInfo.pWaitSemaphores = &imageAquiredSemaphore;
			submitInfo.pWaitDstStageMask = &waitStage;
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers = &frame.CommandBuffer;
			submitInfo.signalSemaphoreCount = 1;
			submitInfo.pSignalSemaphores = &frameSemaphores.RenderCompleteSemaphore;

			CHECK_VK(vkEndCommandBuffer(frame.CommandBuffer));
			CHECK_VK(vkQueueSubmit(myQueue, 1, &submitInfo, frame.Fence));
		}
	}

	void presentFrame()
	{
		auto& window = myWindow.imgui.window;
		auto& fs = window.FrameSemaphores[window.FrameIndex];
		VkPresentInfoKHR info = {};
		info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &fs.RenderCompleteSemaphore;
		info.swapchainCount = 1;
		info.pSwapchains = &myWindow.swapchain.swapchain;
		info.pImageIndices = &window.FrameIndex;
		checkFlipOrPresentResult(vkQueuePresentKHR(myQueue, &info));
	}

	void cleanupFrameResources()
	{
		for (uint32_t frameIt = 0; frameIt < myFrameCount; frameIt++)
		{
			vkDestroyFence(myDevice, myFrameFences[frameIt], nullptr);
			vkDestroySemaphore(myDevice, myImageAcquiredSemaphores[frameIt], nullptr);
			vkDestroySemaphore(myDevice, myRenderCompleteSemaphores[frameIt], nullptr);
		}

		std::vector<VkCommandBuffer> threadCommandBuffers(myFrameCount);
		for (uint32_t cmdIt = 0; cmdIt < myFrameCommandBufferThreadCount; cmdIt++)
		{
			for (uint32_t frameIt = 0; frameIt < myFrameCount; frameIt++)
				threadCommandBuffers[frameIt] =
					myFrameCommandBuffers[myFrameCommandBufferThreadCount * frameIt + cmdIt];

			vkFreeCommandBuffers(
				myDevice, myFrameCommandPools[cmdIt], myFrameCount,
				threadCommandBuffers.data());
			vkDestroyCommandPool(myDevice, myFrameCommandPools[cmdIt], nullptr);
		}

		for (VkFramebuffer framebuffer : myWindow.swapchain.frameBuffers)
			vkDestroyFramebuffer(myDevice, framebuffer, nullptr);

		vkDestroyImageView(myDevice, myWindow.swapchain.depthImageView, nullptr);

		for (VkImageView imageView : myWindow.swapchain.colorImageViews)
			vkDestroyImageView(myDevice, imageView, nullptr);

		vkDestroyRenderPass(myDevice, myPipelineConfig.renderPass, nullptr);

		vkDestroyPipeline(myDevice, myGraphicsPipeline, nullptr);
	}

	void cleanupSwapchain()
	{
		vmaDestroyImage(myAllocator, myWindow.swapchain.depthImage, myWindow.swapchain.depthImageMemory);
		vkDestroySwapchainKHR(myDevice, myWindow.swapchain.swapchain, nullptr);
	}

	void cleanup()
	{
		cleanupFrameResources();
		cleanupSwapchain();

		ImGui_ImplVulkan_Shutdown();
		ImGui::DestroyContext();

		vmaDestroyBuffer(myAllocator, myResources.uniformBuffer, myResources.uniformBufferMemory);

		{
			vmaDestroyBuffer(
				myAllocator, myResources.model.vertexBuffer, myResources.model.vertexBufferMemory);
			vmaDestroyBuffer(
				myAllocator, myResources.model.indexBuffer, myResources.model.indexBufferMemory);

			vmaDestroyImage(
				myAllocator, myResources.texture.image, myResources.texture.imageMemory);
			vkDestroyImageView(myDevice, myResources.texture.imageView, nullptr);
		}

		vkDestroyPipelineLayout(myDevice, myPipelineLayout.layout, nullptr);

		// todo: wrap these in a deleter.
		myPipelineLayout.shaders.reset();
		myPipelineLayout.descriptorSetLayouts.reset();

		vkDestroySampler(myDevice, myResources.sampler, nullptr);

		vkDestroyDescriptorPool(myDevice, myDescriptorPool, nullptr);
		vkDestroyCommandPool(myDevice, myTransferCommandPool, nullptr);

		char* allocatorStatsJSON = nullptr;
		vmaBuildStatsString(myAllocator, &allocatorStatsJSON, true);
		std::cout << allocatorStatsJSON << std::endl;
		vmaFreeStatsString(myAllocator, allocatorStatsJSON);
		vmaDestroyAllocator(myAllocator);

		vkDestroyDevice(myDevice, nullptr);
		vkDestroySurfaceKHR(myInstance, myWindow.surface, nullptr);

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

	Instance<GraphicsBackend::Vulkan> myInstance;
	VkDebugReportCallbackEXT myDebugCallback = VK_NULL_HANDLE;
	PhysicalDevice<GraphicsBackend::Vulkan> myPhysicalDevice;
	Device<GraphicsBackend::Vulkan> myDevice;
	VmaAllocator myAllocator = VK_NULL_HANDLE;
	int myQueueFamilyIndex = -1;
	Queue<GraphicsBackend::Vulkan> myQueue;
	VkDescriptorPool myDescriptorPool = VK_NULL_HANDLE;

	std::vector<VkCommandPool> myFrameCommandPools;		 // count = [threadCount]
	std::vector<VkCommandBuffer> myFrameCommandBuffers;  // count = [frameCount*threadCount] [f0cb0
														 // f0cb1 f1cb0 f1cb1 f2cb0 f2cb1 ...]
	std::vector<VkFence> myFrameFences;					 // count = [frameCount]
	std::vector<VkSemaphore> myImageAcquiredSemaphores;  // count = [frameCount]
	std::vector<VkSemaphore> myRenderCompleteSemaphores; // count = [frameCount]

	VkCommandPool myTransferCommandPool;

	static constexpr uint32_t NX = 2;
	static constexpr uint32_t NY = 1;

	WindowData<GraphicsBackend::Vulkan> myWindow;
	GraphicsPipelineResourceContext<GraphicsBackend::Vulkan> myResources;
	PipelineLayoutContext<GraphicsBackend::Vulkan> myPipelineLayout;
	PipelineConfiguration<GraphicsBackend::Vulkan> myPipelineConfig;
	Pipeline<GraphicsBackend::Vulkan> myGraphicsPipeline; // ~ "PSO"

	using DescriptorSetVector = std::vector<DescriptorSet<GraphicsBackend::Vulkan>>;
	DescriptorSetVector myDescriptorSets;

	std::filesystem::path myResourcePath;

	uint32_t myFrameCount = 0;
	uint32_t myFrameCommandBufferThreadCount = 0;
	int myRequestedCommandBufferThreadCount = 0;

	bool myUIEnableFlag = false;
	bool myCreateFrameResourcesFlag = false;

	std::map<int, bool> myKeysPressed;
	std::array<bool, 2> myMouseButtonsPressed;
	std::array<glm::vec2, 2> myMousePosition;

	std::chrono::high_resolution_clock::time_point myGraphicsFrameTimestamp;
	std::chrono::duration<double> myGraphicsDeltaTime;
};

static VulkanApplication* theApp = nullptr;

int vkapp_create(
	void* view, int width, int height, int framebufferWidth, int framebufferHeight,
	const char* resourcePath, bool verbose)
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

	if (verbose)
	{
		static const char* VK_LOADER_DEBUG_STR = "VK_LOADER_DEBUG";
		if (char* vkLoaderDebug = getenv(VK_LOADER_DEBUG_STR))
			std::cout << VK_LOADER_DEBUG_STR << "=" << vkLoaderDebug << std::endl;

		static const char* VK_LAYER_PATH_STR = "VK_LAYER_PATH";
		if (char* vkLayerPath = getenv(VK_LAYER_PATH_STR))
			std::cout << VK_LAYER_PATH_STR << "=" << vkLayerPath << std::endl;

		static const char* VK_ICD_FILENAMES_STR = "VK_ICD_FILENAMES";
		if (char* vkIcdFilenames = getenv(VK_ICD_FILENAMES_STR))
			std::cout << VK_ICD_FILENAMES_STR << "=" << vkIcdFilenames << std::endl;
	}

	theApp = new VulkanApplication(
		view, width, height, framebufferWidth, framebufferHeight,
		resourcePath ? resourcePath : "./", verbose);

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
