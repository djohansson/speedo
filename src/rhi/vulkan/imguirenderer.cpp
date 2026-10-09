#include <core/profiling.h>
#include <core/utils.h>
#include <rhi/buffer.h>
#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/imguirenderer.h>
#include <rhi/queue.h>
#include <rhi/rhi.h>
#include <rhi/swapchain.h>
#include <rhi/vulkan/utils.h>

#include <algorithm>
#include <cstring>
#include <format>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace rhi
{

namespace imguirenderer
{

// an imgui texture, referenced from ImTextureData::BackendUserData. since the backend would free that as its own type,
// the renderer destroys all of them before shutting the backend down.
struct Texture
{
	Image<kVk> image;
	ImageView<kVk> view;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE; // allocated from the backend's pool, only touched on the ui thread
};

// an upload to a texture (from staging), or, without staging, its destroy
struct TextureOp
{
	Texture* texture = nullptr;
	Buffer<kVk> staging;
	VkBufferImageCopy region{};
	uint64_t sequence = 0; // ui frame sequence when queued
};

} // namespace imguirenderer

template <>
struct ImGuiRenderer<kVk>::State
{
	Device<kVk>& device;
	core::ConcurrentQueue<imguirenderer::TextureOp> textureOps;
	// the queue is only FIFO per producer, and the ui thread is whichever thread the tick task lands on (one at a time)
	core::ProducerToken textureOpsProducer{textureOps};
	std::vector<imguirenderer::TextureOp> deferredTextureDestroys; // draw thread only
	core::ConcurrentQueue<VkDescriptorSet> retiredDescriptorSets; // freed on the ui thread

	explicit State(Device<kVk>& device) : device(device) {}

	static void Destroy(imguirenderer::Texture* texture)
	{
		ImGui_ImplVulkan_RemoveTexture(texture->descriptorSet);
		delete texture; //NOLINT(cppcoreguidelines-owning-memory)
	}

	// creates, updates or destroys tex as imgui requests
	void Update(ImTextureData& tex, uint64_t sequence)
	{
		ZoneScopedN("ImGuiRenderer::UpdateTexture");

		auto* texture = static_cast<imguirenderer::Texture*>(tex.BackendUserData);

		if (tex.Status == ImTextureStatus_WantDestroy)
		{
			// imgui no longer references the texture, but earlier frames may still be drawn: see PrepareFrame
			if (texture != nullptr)
				textureOps.enqueue(textureOpsProducer, imguirenderer::TextureOp{.texture = texture, .sequence = sequence});
			tex.SetTexID(ImTextureID_Invalid);
			tex.BackendUserData = nullptr;
			tex.SetStatus(ImTextureStatus_Destroyed);
			return;
		}

		if (tex.Status == ImTextureStatus_WantCreate)
		{
			ENSURE(texture == nullptr && tex.Format == ImTextureFormat_RGBA32);

			auto width = static_cast<uint32_t>(tex.Width);
			auto height = static_cast<uint32_t>(tex.Height);
			texture = new imguirenderer::Texture{ //NOLINT(cppcoreguidelines-owning-memory) owned by tex.BackendUserData
				.image = Image<kVk>(ImageCreateDesc<kVk>{
					device.CreateDeviceObjectCreateDesc(std::format("ImGui Texture {}", tex.UniqueID)),
					{ImageMipLevelDesc<kVk>{
						.extent = Extent2d{.width = width, .height = height},
						.size = width * height * static_cast<uint32_t>(tex.BytesPerPixel),
						.offset = 0,},},
					Format::kR8G8B8A8Unorm,
					ImageTiling::kOptimal,
					ImageUsage::kSampled | ImageUsage::kTransferDestination,
					MemoryProperty::kDeviceLocal,
					ImageAspect::kColor,
					ImageLayout::kUndefined,}),};
			texture->view = ImageView<kVk>(ImageViewCreateDesc<kVk>{
				device.CreateDeviceObjectCreateDesc(std::format("ImGui Texture View {}", tex.UniqueID)),
				texture->image,
				texture->image.GetDesc().format,
				ImageAspect::kColor});
			texture->descriptorSet = ImGui_ImplVulkan_AddTexture(texture->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			tex.SetTexID(reinterpret_cast<ImTextureID>(texture->descriptorSet));
			tex.BackendUserData = texture;
		}

		if (tex.Status == ImTextureStatus_WantCreate || tex.Status == ImTextureStatus_WantUpdates)
		{
			// imgui only updates regions that were never drawn from, so frames still in flight are unaffected
			bool whole = tex.Status == ImTextureStatus_WantCreate;
			int offsetX = whole ? 0 : tex.UpdateRect.x;
			int offsetY = whole ? 0 : tex.UpdateRect.y;
			int width = whole ? tex.Width : tex.UpdateRect.w;
			int height = whole ? tex.Height : tex.UpdateRect.h;
			auto pitch = static_cast<size_t>(width) * static_cast<size_t>(tex.BytesPerPixel);

			auto staging = Buffer<kVk>::CreateStaging(
				device.CreateDeviceObjectCreateDesc("ImGui Texture Staging"), pitch * static_cast<size_t>(height));
			auto data = staging.Map();
			for (int row = 0; row < height; row++)
				std::memcpy(data.data() + (pitch * static_cast<size_t>(row)), tex.GetPixelsAt(offsetX, offsetY + row), pitch);
			staging.Unmap();

			textureOps.enqueue(
				textureOpsProducer,
				imguirenderer::TextureOp{
					.texture = texture,
					.staging = std::move(staging),
					.region = VkBufferImageCopy{
						.bufferOffset = 0,
						.bufferRowLength = 0,
						.bufferImageHeight = 0,
						.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
						.imageOffset = {.x = offsetX, .y = offsetY, .z = 0},
						.imageExtent = {.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height), .depth = 1}},
					.sequence = sequence});

			// the upload is recorded before any frame published from now on is drawn
			tex.SetStatus(ImTextureStatus_OK);
		}
	}
};

template <>
ImGuiRenderer<kVk>::ImGuiRenderer(Device<kVk>& device, Swapchain<kVk>& swapchain, Queue<kVk>& queue, uint32_t framesInFlight)
	: myState(std::make_unique<State>(device))
{
	ZoneScopedN("ImGuiRenderer()");

	auto& instance = device.GetInstance();
	const auto& surfaceCapabilities =
		instance.GetSwapchainInfo(device.GetPhysicalDevice(), swapchain.GetDesc().surface).capabilities;

	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.Instance = instance;
	initInfo.PhysicalDevice = device.GetPhysicalDevice();
	initInfo.Device = device;
	initInfo.QueueFamily = queue.GetDesc().queueFamilyIndex;
	initInfo.Queue = queue; // required, but only used for texture uploads, which we do ourselves (see Texture)
	initInfo.PipelineCache = device.GetPipeline().GetCache();
	// a pool of the backend's own, used only on the ui thread: the pipeline's pool is used by the draw thread
	initInfo.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
	initInfo.MinImageCount = surfaceCapabilities.minImageCount;
	// how many sets of buffers imgui keeps: as many as frames in flight, and at least as many as swapchain images
	initInfo.ImageCount = std::max<uint32_t>(
		{framesInFlight, static_cast<uint32_t>(swapchain.GetDesc().images.size()), initInfo.MinImageCount});
	initInfo.Allocator = &instance.GetHostAllocationCallbacks();
	initInfo.CheckVkResultFn = [](VkResult result) { VK_CHECK(result); };
	initInfo.MinAllocationSize = 64 * 1024; //NOLINT(readability-magic-numbers)
	initInfo.UseDynamicRendering = swapchain.GetDesc().useDynamicRendering;
	initInfo.PipelineInfoMain.RenderPass = initInfo.UseDynamicRendering ? VK_NULL_HANDLE : swapchain.GetFrames()[0].GetHandle().first;
	initInfo.PipelineInfoMain.Subpass = 0;
	initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfoKHR{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
		.pNext = nullptr,
		.viewMask = 0,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &swapchain.GetDesc().surfaceFormat.format,
	};
	ImGui_ImplVulkan_Init(&initInfo);
}

template <>
ImGuiRenderer<kVk>::~ImGuiRenderer()
{
	ZoneScopedN("~ImGuiRenderer()");

	auto& state = *myState;

	imguirenderer::TextureOp textureOp;
	while (state.textureOps.try_dequeue(textureOp))
		if (!textureOp.staging.IsValid())
			State::Destroy(textureOp.texture);
	for (auto& deferred : state.deferredTextureDestroys)
		State::Destroy(deferred.texture);
	state.deferredTextureDestroys.clear();

	VkDescriptorSet descriptorSet;
	while (state.retiredDescriptorSets.try_dequeue(descriptorSet))
		ImGui_ImplVulkan_RemoveTexture(descriptorSet);

	for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
	{
		if (auto* texture = static_cast<imguirenderer::Texture*>(tex->BackendUserData))
		{
			State::Destroy(texture);
			tex->SetTexID(ImTextureID_Invalid);
			tex->BackendUserData = nullptr;
			tex->SetStatus(ImTextureStatus_Destroyed);
		}
	}

	ImGui_ImplVulkan_Shutdown();
}

template <>
void ImGuiRenderer<kVk>::NewFrame()
{
	ImGui_ImplVulkan_NewFrame();
}

template <>
void ImGuiRenderer<kVk>::UpdateTextures(uint64_t sequence)
{
	ZoneScopedN("ImGuiRenderer::UpdateTextures");

	// descriptor sets of textures the draw thread has retired (see PrepareFrame)
	VkDescriptorSet retiredDescriptorSet;
	while (myState->retiredDescriptorSets.try_dequeue(retiredDescriptorSet))
		ImGui_ImplVulkan_RemoveTexture(retiredDescriptorSet);

	for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
		if (tex->Status != ImTextureStatus_OK)
			myState->Update(*tex, sequence);
}

template <>
void ImGuiRenderer<kVk>::PrepareFrame(
	CommandBufferHandle<kVk> cmd, uint64_t drawnSequence, std::vector<core::TaskHandle>& callbacks)
{
	ZoneScopedN("ImGuiRenderer::PrepareFrame");

	auto& state = *myState;

	std::vector<Buffer<kVk>> stagingBuffers;
	imguirenderer::TextureOp textureOp;
	while (state.textureOps.try_dequeue_from_producer(state.textureOpsProducer, textureOp))
	{
		if (!textureOp.staging.IsValid())
		{
			state.deferredTextureDestroys.emplace_back(std::move(textureOp));
			continue;
		}

		auto& image = textureOp.texture->image;
		image.Transition(cmd, ImageLayout::kTransferDestination, ImageAspect::kColor);
		vkCmdCopyBufferToImage(cmd, textureOp.staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &textureOp.region);
		image.Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);
		stagingBuffers.emplace_back(std::move(textureOp.staging));
	}

	// a texture destroyed on the ui thread may still be referenced by frames published before that, and so by the frame
	// drawn here. from a frame published at or after its destroy on, it is never drawn again, and this submission
	// completes after all earlier ones (the graphics timeline orders them).
	std::vector<std::unique_ptr<imguirenderer::Texture>> textures;
	std::erase_if(
		state.deferredTextureDestroys,
		[&textures, drawnSequence](const imguirenderer::TextureOp& destroy)
		{
			if (destroy.sequence > drawnSequence)
				return false;

			textures.emplace_back(destroy.texture);
			return true;
		});

	if (stagingBuffers.empty() && textures.empty())
		return;

	callbacks.emplace_back(
		core::CreateTask(
			[&state, stagingBuffers = std::make_shared<std::vector<Buffer<kVk>>>(std::move(stagingBuffers)), textures = std::move(textures)]() mutable
			{
				stagingBuffers->clear();

				// descriptor sets come from the backend's pool, which only the ui thread may use
				for (auto& texture : textures)
					state.retiredDescriptorSets.enqueue(texture->descriptorSet);
				textures.clear();
			}).handle);
}

template <>
void ImGuiRenderer<kVk>::Render(ImDrawData& drawData, CommandBufferHandle<kVk> cmd)
{
	ZoneScopedN("ImGuiRenderer::Render");

	ImGui_ImplVulkan_RenderDrawData(&drawData, cmd);
}

} // namespace rhi
