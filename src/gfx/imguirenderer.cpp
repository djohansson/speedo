#include "imguirenderer.h"

#include <gfx/shaderloader.h>
#include <gfx/shaders/capi.h>

#include <core/application.h>
#include <core/assert.h>
#include <core/profiling.h>
#include <core/utils.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <format>
#include <span>
#include <utility>

namespace gfx
{

using namespace rhi;

namespace imguirenderer
{

// the descriptor set of the layout's resources (set 0 holds the push constants)
constexpr uint32_t kSet = 1;
// the sets the pipeline's pool holds: one is bound while the textures don't change, and a change takes a new one (see
// PrepareFrame), the old ones handed back once the gpu is done with them
constexpr uint32_t kSetCount = 128;
// the vertex and index buffers grow to the next power of two at least this big
constexpr size_t kLeastBufferByteCount = size_t{64} * 1024;

[[nodiscard]] std::vector<DescriptorPoolSize> DescriptorPoolSizes()
{
	return {
		{.type = rhi::DescriptorType::kSampledImage, .count = kSetCount * SHADER_TYPES_IMGUI_TEXTURE_COUNT},
		{.type = rhi::DescriptorType::kSampler, .count = kSetCount},
		{.type = rhi::DescriptorType::kStorageBuffer, .count = kSetCount * SHADER_TYPES_FRAME_COUNT},
		// set 0's: the reflection gives the push constants (gImGui) a uniform buffer there, whose sets are allocated too
		{.type = rhi::DescriptorType::kUniformBuffer, .count = kSetCount},
	};
}

// a texture of imgui's (its ImTextureData::BackendUserData), or the default, in a slot
struct Texture
{
	Image image;
	ImageView view;
	uint32_t slot = 0;
};

// what the ui thread asks of the draw thread (see PrepareFrame)
struct Operation
{
	enum class Kind : uint8_t
	{
		kBind, // binds view to slot
		kUpload, // copies staging into region of texture
		kRelease, // binds slot to the default once no frame from sequence on draws it, and releases it (and texture, if any)
	};
	Kind kind = Kind::kBind;
	uint32_t slot = 0;
	ImageViewHandle view{};
	Texture* texture = nullptr; // owned by a release
	Buffer staging;
	Rect region{};
	uint64_t sequence = 0; // the first ui frame that doesn't draw it
};

// a texture's image and view: 8 bits a channel, sampled, uploaded to
[[nodiscard]] std::unique_ptr<Texture> CreateTexture(Device& device, uint32_t width, uint32_t height, uint32_t slot, std::string_view name)
{
	auto texture = std::make_unique<Texture>(Texture{
		.image = Image(ImageCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::string(name)),
			{ImageMipLevelDesc{.extent = Extent2d{.width = width, .height = height}, .size = width * height * 4, .offset = 0}},
			Format::kR8G8B8A8Unorm,
			ImageTiling::kOptimal,
			ImageUsage::kSampled | ImageUsage::kTransferDestination,
			MemoryProperty::kDeviceLocal,
			ImageAspect::kColor,
			ImageLayout::kUndefined}),
		.view = {},
		.slot = slot});
	texture->view = ImageView(ImageViewCreateDesc{
		device.CreateDeviceObjectCreateDesc(std::format("{} View", name)), texture->image, texture->image.GetDesc().format, ImageAspect::kColor});
	return texture;
}

} // namespace imguirenderer

struct ImGuiRenderer::State
{
	Device& device;
	Pipeline pipeline;
	PipelineLayoutHandle layout{};
	SamplerVector sampler;
	std::unique_ptr<imguirenderer::Texture> defaultTexture;

	// in order: binding a slot, uploading to a texture, releasing a slot. the queue is only FIFO per producer, and the ui
	// thread is whichever thread the tick task lands on (one at a time)
	core::ConcurrentQueue<imguirenderer::Operation> operations;
	core::ProducerToken operationsProducer{operations};
	std::vector<imguirenderer::Operation> deferredReleases; // draw thread only

	// the free slots (ui thread only), and those released, once the gpu is done with the frames that drew them
	std::vector<uint32_t> freeSlots;
	core::ConcurrentQueue<uint32_t> retiredSlots;
	uint64_t sequence = 0; // the last ui frame's, see UpdateTextures (ui thread only)

	// per frame slot, the vertices (gImGuiVertices) and indices drawn (draw thread only)
	struct FrameBuffers
	{
		Buffer vertices;
		Buffer indices;
	};
	std::array<FrameBuffers, SHADER_TYPES_FRAME_COUNT> frames;
	bool descriptorsChanged = false; // since the last PrepareFrame handed the sets over (draw thread only)

	explicit State(Device& device)
		: device(device)
		// a pipeline of its own, whose descriptor sets are the ui's, with a pipeline cache of its own: the window's is saved
		// by the device's pipeline, after this one at exit
		, pipeline(PipelineCreateDesc<kGraphicsApi>{
			  device.CreateDeviceObjectCreateDesc("ImGui Pipeline"),
			  (std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / "imgui.pipeline.cache").string(),
			  imguirenderer::DescriptorPoolSizes()})
		// linear, clamped to the edge, as imgui's own backends sample
		, sampler(SamplerVectorCreateDesc{
			  device.CreateDeviceObjectCreateDesc("ImGui Sampler"),
			  {SamplerDesc{
				  .addressModeU = AddressMode::kClampToEdge,
				  .addressModeV = AddressMode::kClampToEdge,
				  .addressModeW = AddressMode::kClampToEdge}}})
	{
		// slot 0 is the default's: the lowest free slot is handed out first
		for (uint32_t slot = SHADER_TYPES_IMGUI_TEXTURE_COUNT - 1; slot > 0; slot--)
			freeSlots.push_back(slot);
	}

	// ui thread
	[[nodiscard]] uint32_t AcquireSlot()
	{
		uint32_t retired = 0;
		while (retiredSlots.try_dequeue(retired))
			freeSlots.push_back(retired);
		ENSUREF(!freeSlots.empty(), "the ui's {} texture slots are all taken (SHADER_TYPES_IMGUI_TEXTURE_COUNT)", SHADER_TYPES_IMGUI_TEXTURE_COUNT);
		auto slot = freeSlots.back();
		freeSlots.pop_back();
		return slot;
	}

	void Enqueue(imguirenderer::Operation&& operation) { operations.enqueue(operationsProducer, std::move(operation)); }

	// creates, updates or destroys tex as imgui asks
	void Update(ImTextureData& tex, uint64_t frameSequence)
	{
		using namespace imguirenderer;

		ZoneScopedN("ImGuiRenderer::UpdateTexture");

		auto* texture = static_cast<Texture*>(tex.BackendUserData);

		if (tex.Status == ImTextureStatus_WantDestroy)
		{
			// imgui no longer draws it, but earlier frames may still be drawn: see PrepareFrame
			if (texture != nullptr)
				Enqueue(Operation{.kind = Operation::Kind::kRelease, .slot = texture->slot, .texture = texture, .sequence = frameSequence});
			tex.SetTexID(ImTextureID_Invalid);
			tex.BackendUserData = nullptr;
			tex.SetStatus(ImTextureStatus_Destroyed);
			return;
		}

		if (tex.Status == ImTextureStatus_WantCreate)
		{
			ENSURE(texture == nullptr && tex.Format == ImTextureFormat_RGBA32);

			texture = CreateTexture(
						  device,
						  static_cast<uint32_t>(tex.Width),
						  static_cast<uint32_t>(tex.Height),
						  AcquireSlot(),
						  std::format("ImGui Texture {}", tex.UniqueID))
						  .release(); // owned by tex.BackendUserData, then by its release
			Enqueue(Operation{.kind = Operation::Kind::kBind, .slot = texture->slot, .view = texture->view});
			tex.SetTexID(static_cast<ImTextureID>(texture->slot));
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

			auto staging = Buffer::CreateStaging(device.CreateDeviceObjectCreateDesc("ImGui Texture Staging"), pitch * static_cast<size_t>(height));
			auto data = staging.Map();
			for (int row = 0; row < height; row++)
				std::memcpy(data.data() + (pitch * static_cast<size_t>(row)), tex.GetPixelsAt(offsetX, offsetY + row), pitch);
			staging.Unmap();

			Enqueue(Operation{
				.kind = Operation::Kind::kUpload,
				.texture = texture,
				.staging = std::move(staging),
				.region = Rect{.x = offsetX, .y = offsetY, .width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)}});

			// the upload is recorded before any frame published from now on is drawn
			tex.SetStatus(ImTextureStatus_OK);
		}
	}
};

ImGuiRenderer::ImGuiRenderer(Device& device)
	: myState(std::make_unique<State>(device))
{
	using namespace imguirenderer;

	ZoneScopedN("ImGuiRenderer()");

	auto& state = *myState;
	auto& variables = core::Application::Get()->GetEnv().variables;
	auto shaderIncludePath = std::get<std::filesystem::path>(variables["RootPath"]) / "src/gfx/shaders";
	ShaderLoader loader({shaderIncludePath}, {}, std::get<std::filesystem::path>(variables["UserProfilePath"]) / ".slang.intermediate");
	state.layout = state.pipeline.CreateLayout(
		"ImGui",
		loader.Load(
			shaderIncludePath / "imgui.slang",
			{
				.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
				.entryPoints = {{"VertexImGui", SLANG_STAGE_VERTEX}, {"FragmentImGui", SLANG_STAGE_FRAGMENT}},
				.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
			}));
	state.pipeline.BindLayoutAuto(state.layout, PipelineBindPoint::kGraphics);
	state.pipeline.SetDescriptorData("gImGuiSampler", ImageBinding{.sampler = state.sampler[0]}, kSet);

	// the default, white, in slot 0: bound and uploaded by the first PrepareFrame
	state.defaultTexture = CreateTexture(device, 1, 1, 0, "ImGui Default Texture");
	auto staging = Buffer::CreateStaging(device.CreateDeviceObjectCreateDesc("ImGui Default Texture Staging"), 4);
	std::ranges::fill(staging.Map(), std::byte{0xff});
	staging.Unmap();
	state.Enqueue(Operation{.kind = Operation::Kind::kBind, .slot = 0, .view = state.defaultTexture->view});
	state.Enqueue(Operation{
		.kind = Operation::Kind::kUpload,
		.texture = state.defaultTexture.get(),
		.staging = std::move(staging),
		.region = Rect{.width = 1, .height = 1}});

	auto& io = ImGui::GetIO();
	io.BackendRendererName = "gfx::ImGuiRenderer";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // ImDrawCmd::VtxOffset is honored: large meshes
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // ImGuiPlatformIO::Textures' requests are (see UpdateTextures)
}

ImGuiRenderer::~ImGuiRenderer()
{
	using namespace imguirenderer;

	ZoneScopedN("~ImGuiRenderer()");

	auto& state = *myState;

	// imgui's textures, wherever they are: queued for the draw thread, deferred by it, or still imgui's
	Operation operation;
	while (state.operations.try_dequeue_from_producer(state.operationsProducer, operation))
		if (operation.kind == Operation::Kind::kRelease)
			state.deferredReleases.push_back(std::move(operation));
	for (auto& release : state.deferredReleases)
		std::unique_ptr<Texture>(release.texture).reset();
	state.deferredReleases.clear();
	for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
	{
		if (auto* texture = static_cast<Texture*>(tex->BackendUserData))
		{
			std::unique_ptr<Texture>(texture).reset();
			tex->SetTexID(ImTextureID_Invalid);
			tex->BackendUserData = nullptr;
			tex->SetStatus(ImTextureStatus_Destroyed);
		}
	}

	auto& io = ImGui::GetIO();
	io.BackendRendererName = nullptr;
	io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
}

void ImGuiRenderer::UpdateTextures(uint64_t sequence)
{
	ZoneScopedN("ImGuiRenderer::UpdateTextures");

	myState->sequence = sequence;
	for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
		if (tex->Status != ImTextureStatus_OK)
			myState->Update(*tex, sequence);
}

void ImGuiRenderer::PrepareFrame(
	CommandBufferHandle cmd, const ImDrawData& drawData, uint64_t drawnSequence, uint32_t frameIndex, std::vector<core::TaskHandle>& callbacks)
{
	using namespace imguirenderer;

	ZoneScopedN("ImGuiRenderer::PrepareFrame");

	auto& state = *myState;
	auto& pipeline = state.pipeline;
	CommandEncoder encoder(cmd);
	pipeline.BindLayoutAuto(state.layout, PipelineBindPoint::kGraphics);

	auto bind = [&pipeline, &state](uint32_t slot, ImageViewHandle view)
	{
		pipeline.SetDescriptorData("gImGuiTextures", ImageBinding{.sampler = {}, .imageView = view, .layout = ImageLayout::kShaderReadOnly}, kSet, slot);
		state.descriptorsChanged = true;
	};

	// what the ui thread asked for: uploads recorded now, before the frames that draw them
	std::vector<Buffer> stagingBuffers;
	Operation operation;
	while (state.operations.try_dequeue_from_producer(state.operationsProducer, operation))
	{
		switch (operation.kind)
		{
		case Operation::Kind::kBind:
			bind(operation.slot, operation.view);
			break;
		case Operation::Kind::kUpload:
		{
			auto& image = operation.texture->image;
			image.Transition(cmd, ImageLayout::kTransferDestination, ImageAspect::kColor);
			encoder.CopyBufferToImage(operation.staging, image, operation.region);
			image.Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);
			stagingBuffers.emplace_back(std::move(operation.staging));
			break;
		}
		case Operation::Kind::kRelease:
			state.deferredReleases.emplace_back(std::move(operation));
			break;
		}
	}

	// a slot released on the ui thread may still be drawn by frames published before that, and so by the frame drawn here.
	// from a frame published at or after its release on, it is never drawn again: its slot is bound to the default from
	// here on, and the texture and the slot go once this submission completes, after all earlier ones (the graphics
	// timeline orders them)
	std::vector<std::unique_ptr<Texture>> textures;
	std::vector<uint32_t> slots;
	std::erase_if(
		state.deferredReleases,
		[&](const Operation& release)
		{
			if (release.sequence > drawnSequence)
				return false;

			bind(release.slot, state.defaultTexture->view);
			textures.emplace_back(release.texture);
			slots.push_back(release.slot);
			return true;
		});

	// the frame's vertices and indices, into the frame slot's buffers (its last frame is done), grown to hold them: the old
	// ones go once this submission completes (other frame slots' frames in flight bound sets naming them)
	std::vector<Buffer> retiredBuffers;
	auto& frame = state.frames[frameIndex];
	if (drawData.TotalVtxCount > 0)
	{
		auto vertexByteCount = static_cast<size_t>(drawData.TotalVtxCount) * sizeof(ImDrawVert);
		auto indexByteCount = static_cast<size_t>(drawData.TotalIdxCount) * sizeof(ImDrawIdx);
		auto grow = [&](Buffer& buffer, size_t byteCount, BufferUsage usage, std::string_view what)
		{
			if (buffer.IsValid() && buffer.GetDesc().size >= byteCount)
				return false;
			if (buffer.IsValid())
				retiredBuffers.emplace_back(std::move(buffer));
			buffer = Buffer(BufferCreateDesc{
				state.device.CreateDeviceObjectCreateDesc(std::format("ImGui {} {}", what, frameIndex)),
				std::bit_ceil(std::max(byteCount, kLeastBufferByteCount)),
				usage,
				MemoryProperty::kHostVisible});
			return true;
		};
		if (grow(frame.vertices, vertexByteCount, BufferUsage::kStorage, "Vertices"))
		{
			pipeline.SetDescriptorData("gImGuiVertices", BufferBinding{.buffer = frame.vertices, .offset = 0}, kSet, frameIndex);
			state.descriptorsChanged = true;
		}
		grow(frame.indices, indexByteCount, BufferUsage::kIndex, "Indices");

		auto vertices = frame.vertices.Map();
		auto indices = frame.indices.Map();
		size_t vertexOffset = 0;
		size_t indexOffset = 0;
		for (const ImDrawList* drawList : drawData.CmdLists)
		{
			auto vertexBytes = std::as_bytes(std::span(drawList->VtxBuffer.Data, static_cast<size_t>(drawList->VtxBuffer.Size)));
			auto indexBytes = std::as_bytes(std::span(drawList->IdxBuffer.Data, static_cast<size_t>(drawList->IdxBuffer.Size)));
			std::ranges::copy(vertexBytes, vertices.begin() + static_cast<ptrdiff_t>(vertexOffset));
			std::ranges::copy(indexBytes, indices.begin() + static_cast<ptrdiff_t>(indexOffset));
			vertexOffset += vertexBytes.size();
			indexOffset += indexBytes.size();
		}
		frame.vertices.Flush(0, vertexOffset);
		frame.indices.Flush(0, indexOffset);
		frame.vertices.Unmap();
		frame.indices.Unmap();
	}

	// a set holds every slot, and a bind of changed ones takes a new set from the pool, which never has it back: the sets so
	// far go once this submission completes (the frames before drew with them, this one binds a new one)
	std::shared_ptr<void> sets;
	if (std::exchange(state.descriptorsChanged, false))
		sets = pipeline.TakeDescriptorSets(kSet);

	if (stagingBuffers.empty() && textures.empty() && retiredBuffers.empty() && !sets)
		return;

	callbacks.emplace_back(
		core::CreateTask(
			[&state,
			 buffers = std::make_shared<std::vector<Buffer>>(std::move(stagingBuffers)),
			 retired = std::make_shared<std::vector<Buffer>>(std::move(retiredBuffers)),
			 textures = std::move(textures),
			 slots = std::move(slots),
			 sets = std::move(sets)]() mutable
			{
				buffers->clear();
				retired->clear();
				textures.clear();
				sets.reset();
				for (auto slot : slots)
					state.retiredSlots.enqueue(slot);
			}).handle);
}

void ImGuiRenderer::Draw(CommandBufferHandle cmd, const ImDrawData& drawData, uint32_t frameIndex, IRenderTarget& target)
{
	ZoneScopedN("ImGuiRenderer::Draw");

	// in the framebuffer's pixels
	auto framebufferWidth = static_cast<int>(drawData.DisplaySize.x * drawData.FramebufferScale.x);
	auto framebufferHeight = static_cast<int>(drawData.DisplaySize.y * drawData.FramebufferScale.y);
	if (framebufferWidth <= 0 || framebufferHeight <= 0 || drawData.TotalVtxCount == 0)
		return;

	auto& state = *myState;
	auto& pipeline = state.pipeline;
	auto& frame = state.frames[frameIndex];
	CommandEncoder encoder(cmd);

	ImGuiConstants constants{};
	constants.scale[0] = 2.0F / drawData.DisplaySize.x;
	constants.scale[1] = 2.0F / drawData.DisplaySize.y;
	constants.translate[0] = -1.0F - (drawData.DisplayPos.x * constants.scale[0]);
	constants.translate[1] = -1.0F - (drawData.DisplayPos.y * constants.scale[1]);
	constants.frame = frameIndex;
	auto setup = [&]
	{
		pipeline.SetRenderTarget(target);
		pipeline.BindLayoutAuto(state.layout, PipelineBindPoint::kGraphics);
		pipeline.BindDescriptorSetAuto(cmd, imguirenderer::kSet);
		pipeline.BindPipelineAuto(cmd, GraphicsPipelineVariant{.blend = {BlendMode::kAlpha}});
		encoder.SetViewport(Viewport{.width = static_cast<float>(framebufferWidth), .height = static_cast<float>(framebufferHeight)});
		encoder.SetCullMode(CullMode::kNone);
		encoder.SetFrontFace(FrontFace::kCounterClockwise);
		encoder.SetStencilReference(0);
		encoder.BindIndexBuffer(frame.indices, 0, sizeof(ImDrawIdx) == 2 ? IndexType::kUint16 : IndexType::kUint32);
		constants.texture = ~0U; // pushed with the first draw's
	};
	setup();

	// clip rectangles from imgui's coordinates to the framebuffer's
	auto clipOffset = drawData.DisplayPos;
	auto clipScale = drawData.FramebufferScale;
	uint32_t vertexBase = 0;
	uint32_t indexBase = 0;
	for (const ImDrawList* drawList : drawData.CmdLists)
	{
		for (const ImDrawCmd& command : drawList->CmdBuffer)
		{
			if (command.UserCallback != nullptr)
			{
				if (command.UserCallback == ImDrawCallback_ResetRenderState)
					setup();
				else
					command.UserCallback(drawList, &command);
				continue;
			}

			float left = std::max((command.ClipRect.x - clipOffset.x) * clipScale.x, 0.0F);
			float top = std::max((command.ClipRect.y - clipOffset.y) * clipScale.y, 0.0F);
			float right = std::min((command.ClipRect.z - clipOffset.x) * clipScale.x, static_cast<float>(framebufferWidth));
			float bottom = std::min((command.ClipRect.w - clipOffset.y) * clipScale.y, static_cast<float>(framebufferHeight));
			if (right <= left || bottom <= top)
				continue;
			encoder.SetScissor(Rect{
				.x = static_cast<int32_t>(left),
				.y = static_cast<int32_t>(top),
				.width = static_cast<uint32_t>(right - left),
				.height = static_cast<uint32_t>(bottom - top)});

			// the draw's texture and first vertex, pushed where they change (a draw list's draws share their vertices)
			auto texture = static_cast<uint32_t>(command.GetTexID());
			auto firstVertex = command.VtxOffset + vertexBase;
			if (texture != constants.texture || firstVertex != constants.vertexBase)
			{
				constants.texture = texture;
				constants.vertexBase = firstVertex;
				pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
			}

			encoder.DrawIndexed(command.ElemCount, 1, command.IdxOffset + indexBase, 0, 0);
		}
		vertexBase += static_cast<uint32_t>(drawList->VtxBuffer.Size);
		indexBase += static_cast<uint32_t>(drawList->IdxBuffer.Size);
	}
}

ImTextureID ImGuiRenderer::AddTexture(ImageViewHandle view)
{
	auto slot = myState->AcquireSlot();
	myState->Enqueue(imguirenderer::Operation{.kind = imguirenderer::Operation::Kind::kBind, .slot = slot, .view = view});
	return static_cast<ImTextureID>(slot);
}

void ImGuiRenderer::RemoveTexture(ImTextureID id)
{
	// the frame being built is published as the next sequence, and doesn't draw it
	myState->Enqueue(imguirenderer::Operation{
		.kind = imguirenderer::Operation::Kind::kRelease, .slot = static_cast<uint32_t>(id), .sequence = myState->sequence + 1});
}

} // namespace gfx
