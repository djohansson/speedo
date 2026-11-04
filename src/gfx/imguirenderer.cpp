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
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <span>
#include <type_traits>
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

// what the renderer takes imgui's types to be: a change of them (a newer imgui, or its configuration in imconfig.h) fails
// the build here, rather than drawing garbage
static_assert(IMGUI_VERSION_NUM >= 19200, "the renderer handles imgui's textures as 1.92 asks (ImGuiBackendFlags_RendererHasTextures)");
// its vertices, as capi.h lays them out for the shader (SHADER_TYPES_IMGUI_VERTEX_*: 32 bit words)
static_assert(
	std::is_same_v<decltype(ImVec2::x), float> && std::is_same_v<decltype(ImVec2::y), float> && sizeof(ImVec2) == 2 * sizeof(float),
	"ImVec2 isn't two floats");
static_assert(
	std::is_same_v<decltype(ImDrawVert::pos), ImVec2> && offsetof(ImDrawVert, pos) == SHADER_TYPES_IMGUI_VERTEX_POSITION_WORD * sizeof(uint32_t),
	"ImDrawVert's position isn't an ImVec2 where SHADER_TYPES_IMGUI_VERTEX_POSITION_WORD says");
static_assert(
	std::is_same_v<decltype(ImDrawVert::uv), ImVec2> && offsetof(ImDrawVert, uv) == SHADER_TYPES_IMGUI_VERTEX_UV_WORD * sizeof(uint32_t),
	"ImDrawVert's texture coordinates aren't an ImVec2 where SHADER_TYPES_IMGUI_VERTEX_UV_WORD says");
static_assert(
	std::is_same_v<decltype(ImDrawVert::col), ImU32> && std::is_same_v<ImU32, uint32_t> &&
		offsetof(ImDrawVert, col) == SHADER_TYPES_IMGUI_VERTEX_COLOR_WORD * sizeof(uint32_t),
	"ImDrawVert's color isn't a 32 bit word where SHADER_TYPES_IMGUI_VERTEX_COLOR_WORD says");
static_assert(sizeof(ImDrawVert) == SHADER_TYPES_IMGUI_VERTEX_WORD_COUNT * sizeof(uint32_t), "ImDrawVert isn't SHADER_TYPES_IMGUI_VERTEX_WORD_COUNT words");
// its colors' channels (IM_COL32, other with IMGUI_USE_BGRA_PACKED_COLOR), where the shader unpacks them
static_assert(
	IM_COL32_R_SHIFT == SHADER_TYPES_IMGUI_COLOR_R_SHIFT && IM_COL32_G_SHIFT == SHADER_TYPES_IMGUI_COLOR_G_SHIFT &&
		IM_COL32_B_SHIFT == SHADER_TYPES_IMGUI_COLOR_B_SHIFT && IM_COL32_A_SHIFT == SHADER_TYPES_IMGUI_COLOR_A_SHIFT,
	"imgui packs its colors' channels elsewhere than SHADER_TYPES_IMGUI_COLOR_*_SHIFT");
// its indices, bound as 16 or 32 bit ones (see Draw)
static_assert(std::is_unsigned_v<ImDrawIdx> && (sizeof(ImDrawIdx) == 2 || sizeof(ImDrawIdx) == 4), "ImDrawIdx isn't a 16 or 32 bit index");
// its texture ids, a slot and its generation (see TextureId), none 0
static_assert(std::is_integral_v<ImTextureID> && sizeof(ImTextureID) >= sizeof(uint64_t), "ImTextureID can't hold a slot and a generation");
static_assert(ImTextureID_Invalid == 0, "slot 0's id, generation 0, must be imgui's invalid one");
// the push constants as the shader lays them out (float2s 8 bytes aligned, then the words)
static_assert(
	offsetof(ImGuiConstants, scale) == 0 && offsetof(ImGuiConstants, translate) == 8 && offsetof(ImGuiConstants, frame) == 16 &&
		offsetof(ImGuiConstants, texture) == 20 && offsetof(ImGuiConstants, vertexBase) == 24 && sizeof(ImGuiConstants) == 32,
	"ImGuiConstants isn't laid out as the shader's push constants");

// a texture's id (ImTextureID): its slot in the low 32 bits, and its generation in the high ones, a new one for each
// texture a slot holds, so that a draw of an id its slot no longer holds (released, or held by another texture since)
// fails rather than drawing another texture (see Draw). generation 0 is none's
[[nodiscard]] constexpr ImTextureID TextureId(uint32_t slot, uint32_t generation) noexcept
{
	return static_cast<ImTextureID>((uint64_t{generation} << 32U) | slot);
}
[[nodiscard]] constexpr uint32_t SlotOf(ImTextureID id) noexcept
{
	return static_cast<uint32_t>(static_cast<uint64_t>(id));
}
[[nodiscard]] constexpr uint32_t GenerationOf(ImTextureID id) noexcept
{
	return static_cast<uint32_t>(static_cast<uint64_t>(id) >> 32U);
}

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

// a texture of imgui's (its ImTextureData::BackendUserData), or the default, in a slot (of the id, see TextureId)
struct Texture
{
	Image image;
	ImageView view;
	ImTextureID id = ImTextureID_Invalid;
};

// what the ui thread asks of the draw thread (see PrepareFrame)
struct Operation
{
	enum class Kind : uint8_t
	{
		kBind, // binds view to id's slot
		kUpload, // copies staging into region of texture
		kRelease, // binds id's slot to the default once no frame from sequence on draws it, and frees it (and texture, if any)
	};
	Kind kind = Kind::kBind;
	ImTextureID id = ImTextureID_Invalid;
	ImageViewHandle view{};
	Texture* texture = nullptr; // owned by a release
	Buffer staging;
	Rect region{};
	uint64_t sequence = 0; // the first ui frame that doesn't draw it
};

// a texture's image and view: 8 bits a channel, sampled, uploaded to
[[nodiscard]] std::unique_ptr<Texture> CreateTexture(Device& device, uint32_t width, uint32_t height, ImTextureID id, std::string_view name)
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
		.id = id});
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
	// per slot, the generation of the texture that holds it (0: none) and of the last one that did, as the ui thread hands
	// them out and as the draw thread has bound them (see TextureId)
	std::array<uint32_t, SHADER_TYPES_IMGUI_TEXTURE_COUNT> heldGenerations{}; // ui thread only
	std::array<uint32_t, SHADER_TYPES_IMGUI_TEXTURE_COUNT> lastGenerations{}; // ui thread only
	std::array<uint32_t, SHADER_TYPES_IMGUI_TEXTURE_COUNT> boundGenerations{}; // draw thread only
	uint64_t sequence = 0; // the last ui frame's, see UpdateTextures (ui thread only)

	// per frame slot, the vertices (gImGuiVertices) and indices drawn (draw thread only)
	struct FrameBuffers
	{
		Buffer vertices;
		Buffer indices;
	};
	std::array<FrameBuffers, SHADER_TYPES_FRAME_COUNT> frames;
	// per frame slot, the draw data PrepareFrame copied into its buffers, and how many vertices and indices: what Draw must
	// be given (draw thread only)
	struct Prepared
	{
		const ImDrawData* drawData = nullptr;
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
	};
	std::array<Prepared, SHADER_TYPES_FRAME_COUNT> prepared;
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

	// a free slot, held by a new generation: the id of the texture it is taken for. ui thread
	[[nodiscard]] ImTextureID AcquireId()
	{
		uint32_t retired = 0;
		while (retiredSlots.try_dequeue(retired))
			freeSlots.push_back(retired);
		ENSUREF(!freeSlots.empty(), "the ui's {} texture slots are all taken (SHADER_TYPES_IMGUI_TEXTURE_COUNT)", SHADER_TYPES_IMGUI_TEXTURE_COUNT);
		auto slot = freeSlots.back();
		freeSlots.pop_back();
		auto generation = ++lastGenerations[slot];
		if (generation == 0) // past 2^32 textures in the slot, around again
			generation = ++lastGenerations[slot];
		heldGenerations[slot] = generation;
		return imguirenderer::TextureId(slot, generation);
	}

	// the release of id, which must hold its slot: once no frame from sequence on draws it (and texture, if given). ui
	// thread
	void Release(ImTextureID id, imguirenderer::Texture* texture, uint64_t releaseSequence)
	{
		using namespace imguirenderer;

		auto slot = SlotOf(id);
		ENSUREF(
			slot < SHADER_TYPES_IMGUI_TEXTURE_COUNT && GenerationOf(id) != 0 && heldGenerations[slot] == GenerationOf(id),
			"the ui releases texture {:#x}, which isn't one of the renderer's (or was released already)",
			static_cast<uint64_t>(id));
		heldGenerations[slot] = 0;
		Enqueue(Operation{.kind = Operation::Kind::kRelease, .id = id, .texture = texture, .sequence = releaseSequence});
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
				Release(texture->id, texture, frameSequence);
			tex.SetTexID(ImTextureID_Invalid);
			tex.BackendUserData = nullptr;
			tex.SetStatus(ImTextureStatus_Destroyed);
			return;
		}

		if (tex.Status == ImTextureStatus_WantCreate)
		{
			ENSURE(texture == nullptr);
			// the textures are 8 bits a channel, rgba (the font atlas's, unless imgui is configured otherwise)
			ENSUREF(
				tex.Format == ImTextureFormat_RGBA32 && tex.BytesPerPixel == 4,
				"imgui asks for a texture of format {} ({} bytes a texel), the renderer has rgba ones alone",
				static_cast<int>(tex.Format),
				tex.BytesPerPixel);

			texture = CreateTexture(
						  device,
						  static_cast<uint32_t>(tex.Width),
						  static_cast<uint32_t>(tex.Height),
						  AcquireId(),
						  std::format("ImGui Texture {}", tex.UniqueID))
						  .release(); // owned by tex.BackendUserData, then by its release
			Enqueue(Operation{.kind = Operation::Kind::kBind, .id = texture->id, .view = texture->view});
			tex.SetTexID(texture->id);
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
	state.defaultTexture = CreateTexture(device, 1, 1, TextureId(0, 0), "ImGui Default Texture");
	auto staging = Buffer::CreateStaging(device.CreateDeviceObjectCreateDesc("ImGui Default Texture Staging"), 4);
	std::ranges::fill(staging.Map(), std::byte{0xff});
	staging.Unmap();
	state.Enqueue(Operation{.kind = Operation::Kind::kBind, .id = state.defaultTexture->id, .view = state.defaultTexture->view});
	state.Enqueue(Operation{
		.kind = Operation::Kind::kUpload,
		.texture = state.defaultTexture.get(),
		.staging = std::move(staging),
		.region = Rect{.width = 1, .height = 1}});

	auto& io = ImGui::GetIO();
	io.BackendRendererName = "gfx::ImGuiRenderer";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // ImDrawCmd::VtxOffset is honored: large meshes
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // ImGuiPlatformIO::Textures' requests are (see UpdateTextures)
	// no texture bigger than the device takes (imgui's font atlas grows as glyphs are drawn)
	auto& platformIo = ImGui::GetPlatformIO();
	platformIo.Renderer_TextureMaxWidth = static_cast<int>(device.GetLimits().maxImageDimension2d);
	platformIo.Renderer_TextureMaxHeight = static_cast<int>(device.GetLimits().maxImageDimension2d);
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

	ENSUREF(frameIndex < SHADER_TYPES_FRAME_COUNT, "frame slot {} of {}", frameIndex, SHADER_TYPES_FRAME_COUNT);

	// binds id's view to its slot, which draws of it may name from then on (see Draw)
	auto bind = [&pipeline, &state](ImTextureID id, ImageViewHandle view)
	{
		pipeline.SetDescriptorData(
			"gImGuiTextures", ImageBinding{.sampler = {}, .imageView = view, .layout = ImageLayout::kShaderReadOnly}, kSet, SlotOf(id));
		state.boundGenerations[SlotOf(id)] = GenerationOf(id);
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
			bind(operation.id, operation.view);
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

			ENSURE(state.boundGenerations[SlotOf(release.id)] == GenerationOf(release.id));
			// bound to the default's view, and held by none: a draw naming it fails (see Draw)
			pipeline.SetDescriptorData(
				"gImGuiTextures",
				ImageBinding{.sampler = {}, .imageView = state.defaultTexture->view, .layout = ImageLayout::kShaderReadOnly},
				kSet,
				SlotOf(release.id));
			state.boundGenerations[SlotOf(release.id)] = 0;
			state.descriptorsChanged = true;
			textures.emplace_back(release.texture);
			slots.push_back(SlotOf(release.id));
			return true;
		});

	// the frame's vertices and indices, into the frame slot's buffers (its last frame is done), grown to hold them: the old
	// ones go once this submission completes (other frame slots' frames in flight bound sets naming them)
	// counted from the lists, which are copied, and checked against imgui's counts (the snapshot keeps them, see
	// platform::ImGuiLayer): a frame of other counts than its lists' wrote past its buffers
	size_t vertexCount = 0;
	size_t indexCount = 0;
	for (const ImDrawList* drawList : drawData.CmdLists)
	{
		vertexCount += static_cast<size_t>(drawList->VtxBuffer.Size);
		indexCount += static_cast<size_t>(drawList->IdxBuffer.Size);
	}
	ENSUREF(
		vertexCount == static_cast<size_t>(drawData.TotalVtxCount) && indexCount == static_cast<size_t>(drawData.TotalIdxCount),
		"the ui frame's lists hold {} vertices and {} indices, its counts say {} and {}",
		vertexCount,
		indexCount,
		drawData.TotalVtxCount,
		drawData.TotalIdxCount);
	state.prepared[frameIndex] = State::Prepared{
		.drawData = &drawData, .vertexCount = static_cast<uint32_t>(vertexCount), .indexCount = static_cast<uint32_t>(indexCount)};

	std::vector<Buffer> retiredBuffers;
	auto& frame = state.frames[frameIndex];
	if (vertexCount > 0)
	{
		auto vertexByteCount = vertexCount * sizeof(ImDrawVert);
		auto indexByteCount = indexCount * sizeof(ImDrawIdx);
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
	using namespace imguirenderer;

	ZoneScopedN("ImGuiRenderer::Draw");

	auto& state = *myState;

	// what PrepareFrame copied into the frame slot's buffers, before (the UI Uploads pass): else its vertices are another
	// frame's, or none
	ENSUREF(frameIndex < SHADER_TYPES_FRAME_COUNT, "frame slot {} of {}", frameIndex, SHADER_TYPES_FRAME_COUNT);
	const auto& prepared = state.prepared[frameIndex];
	ENSUREF(
		prepared.drawData == &drawData && prepared.vertexCount == static_cast<uint32_t>(drawData.TotalVtxCount) &&
			prepared.indexCount == static_cast<uint32_t>(drawData.TotalIdxCount),
		"the ui draws a frame PrepareFrame didn't prepare in frame slot {}",
		frameIndex);

	// in the framebuffer's pixels
	auto framebufferWidth = static_cast<int>(drawData.DisplaySize.x * drawData.FramebufferScale.x);
	auto framebufferHeight = static_cast<int>(drawData.DisplaySize.y * drawData.FramebufferScale.y);
	if (framebufferWidth <= 0 || framebufferHeight <= 0 || drawData.TotalVtxCount == 0)
		return;

	auto& pipeline = state.pipeline;
	auto& frame = state.frames[frameIndex];
	CommandEncoder encoder(cmd);

	ImGuiConstants constants{};
	constants.scale[0] = 2.0F / drawData.DisplaySize.x;
	constants.scale[1] = 2.0F / drawData.DisplaySize.y;
	constants.translate[0] = -1.0F - (drawData.DisplayPos.x * constants.scale[0]);
	constants.translate[1] = -1.0F - (drawData.DisplayPos.y * constants.scale[1]);
	constants.frame = frameIndex;
	bool pushed = false; // the constants, since the render state was set up or a callback ran
	auto setup = [&]
	{
		pipeline.SetRenderTarget(target);
		pipeline.BindLayoutAuto(state.layout, PipelineBindPoint::kGraphics);
		pipeline.BindDescriptorSetAuto(cmd, kSet);
		pipeline.BindPipelineAuto(cmd, GraphicsPipelineVariant{.blend = {BlendMode::kAlpha}});
		// every dynamic state rhi's graphics pipelines have
		encoder.SetViewport(Viewport{.width = static_cast<float>(framebufferWidth), .height = static_cast<float>(framebufferHeight)});
		encoder.SetCullMode(CullMode::kNone);
		encoder.SetFrontFace(FrontFace::kCounterClockwise);
		encoder.SetStencilReference(0);
		encoder.BindIndexBuffer(frame.indices, 0, sizeof(ImDrawIdx) == 2 ? IndexType::kUint16 : IndexType::kUint32);
		pushed = false;
	};
	setup();

	// clip rectangles from imgui's coordinates to the framebuffer's
	auto clipOffset = drawData.DisplayPos;
	auto clipScale = drawData.FramebufferScale;
	uint32_t vertexBase = 0;
	uint32_t indexBase = 0;
	for (const ImDrawList* drawList : drawData.CmdLists)
	{
		auto listVertexCount = static_cast<uint32_t>(drawList->VtxBuffer.Size);
		auto listIndexCount = static_cast<uint32_t>(drawList->IdxBuffer.Size);
		for (const ImDrawCmd& command : drawList->CmdBuffer)
		{
			if (command.UserCallback != nullptr)
			{
				// a callback of the application's may change the render state (imgui's own backends assume it leaves the
				// pipeline as it was): the constants are pushed again, or all of it set up again where it asks
				if (command.UserCallback == ImDrawCallback_ResetRenderState)
					setup();
				else
					command.UserCallback(drawList, &command);
				pushed = false;
				continue;
			}
			if (command.ElemCount == 0)
				continue;

			// within its list's vertices and indices: out of them, the gpu reads past the frame's buffers
			ENSUREF(
				command.IdxOffset + command.ElemCount <= listIndexCount && command.VtxOffset < listVertexCount,
				"a ui draw of {} indices from {} and vertices from {} past its list's {} indices and {} vertices",
				command.ElemCount,
				command.IdxOffset,
				command.VtxOffset,
				listIndexCount,
				listVertexCount);

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

			// a texture of the renderer's that its slot still holds (bound by PrepareFrame, not released since): another id
			// would draw whatever the slot holds, or past the textures (a lost device)
			auto id = command.GetTexID();
			auto slot = SlotOf(id);
			ENSUREF(
				slot < SHADER_TYPES_IMGUI_TEXTURE_COUNT && GenerationOf(id) != 0 && state.boundGenerations[slot] == GenerationOf(id),
				"the ui draws texture {:#x}, which isn't one of the renderer's (or no longer is: released, see RemoveTexture)",
				static_cast<uint64_t>(id));

			// the draw's texture and first vertex, pushed where they change (a draw list's draws share their vertices)
			auto firstVertex = command.VtxOffset + vertexBase;
			if (!pushed || slot != constants.texture || firstVertex != constants.vertexBase)
			{
				constants.texture = slot;
				constants.vertexBase = firstVertex;
				pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
				pushed = true;
			}

			encoder.DrawIndexed(command.ElemCount, 1, command.IdxOffset + indexBase, 0, 0);
		}
		vertexBase += listVertexCount;
		indexBase += listIndexCount;
	}
}

ImTextureID ImGuiRenderer::AddTexture(ImageViewHandle view)
{
	auto id = myState->AcquireId();
	myState->Enqueue(imguirenderer::Operation{.kind = imguirenderer::Operation::Kind::kBind, .id = id, .view = view});
	return id;
}

void ImGuiRenderer::RemoveTexture(ImTextureID id)
{
	// the frame being built is published as the next sequence, and doesn't draw it
	myState->Release(id, nullptr, myState->sequence + 1);
}

} // namespace gfx
