#include "imguilayer.h"

#include <core/application.h>
#include <core/profiling.h>
#include <platform/imguiplatform.h>

#include <algorithm>
#include <cstring>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include <imgui_freetype.h>
#include <imgui_internal.h> // ImGuiWindow, for the frosted panels (see EndFrame)

namespace gfx
{

namespace
{

constexpr float kDefaultFontSize = 16.0F;

} // namespace

ImGuiLayer::ImGuiLayer(
	const platform::Window& window,
	Swapchain& swapchain,
	RHI& rhi,
	Queue& graphicsQueue,
	uint32_t graphicsQueueCount,
	std::string_view imguiIniSettings)
{
	ZoneScopedN("ImGuiLayer::Init");

	using namespace ImGui;

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
	float dpiScaleX = window.GetState().xscale;
	float dpiScaleY = window.GetState().yscale;
#endif

	imguiIO.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

	// menus and popups as macOS's: rounded, with a faint border (scaled with the rest below; their colors further down)
	GetStyle().PopupRounding = 8.0F;
	GetStyle().PopupBorderSize = 1.0F;
	GetStyle().ScaleAllSizes(std::max(dpiScaleX, dpiScaleY));

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.RasterizerDensity = std::max(window.GetState().xscale, window.GetState().yscale);
	config.PixelSnapH = false;

	imguiIO.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	auto fontDirectory = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]) / "fonts";
	const float fontSize = kDefaultFontSize * std::max(dpiScaleX, dpiScaleY);

	// Roboto, and what it lacks from the fonts merged into it, in order (imgui loads glyphs as they are first drawn, so a
	// fallback costs nothing until it is used): Twemoji's color emoji (COLR layers, which imgui's freetype loader draws
	// with LoadColor; before the symbols, which have some of the same characters in one color), Noto's symbols, and a
	// CJK font of the system's if it has one (too big to bundle)
	ImFont* defaultFont = imguiIO.Fonts->AddFontFromFileTTF((fontDirectory / "Roboto-Medium.ttf").generic_string().c_str(), fontSize, &config);

	ImFontConfig merge = config;
	merge.MergeMode = true;
	{
		ImFontConfig emoji = merge;
		emoji.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
		imguiIO.Fonts->AddFontFromFileTTF((fontDirectory / "Twemoji.Mozilla.ttf").generic_string().c_str(), fontSize, &emoji);
	}
	imguiIO.Fonts->AddFontFromFileTTF((fontDirectory / "NotoSansSymbols2-Regular.ttf").generic_string().c_str(), fontSize, &merge);

	constexpr std::array<std::string_view, 6> kCjkFonts{{
		"C:/Windows/Fonts/msyh.ttc", // Microsoft YaHei
		"C:/Windows/Fonts/YuGothM.ttc", // Yu Gothic
		"/System/Library/Fonts/PingFang.ttc",
		"/System/Library/Fonts/Hiragino Sans GB.ttc",
		"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", // debian, ubuntu
		"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", // fedora
	}};
	for (auto cjk : kCjkFonts)
	{
		std::error_code error;
		if (std::filesystem::exists(cjk, error))
		{
			imguiIO.Fonts->AddFontFromFileTTF(std::string(cjk).c_str(), fontSize, &merge);
			break;
		}
	}

	// Setup style
	StyleColorsClassic();
	// the frosted panels' (see EndFrame): translucent over their blurred backdrop, with a faint edge, and macOS's blue for
	// the item under the mouse and the open menu's
	auto& colors = GetStyle().Colors;
	colors[ImGuiCol_MenuBarBg] = ImVec4(0.10F, 0.10F, 0.12F, 0.45F);
	colors[ImGuiCol_PopupBg] = ImVec4(0.12F, 0.12F, 0.14F, 0.55F);
	colors[ImGuiCol_Border] = ImVec4(1.0F, 1.0F, 1.0F, 0.14F);
	colors[ImGuiCol_Header] = ImVec4(0.04F, 0.40F, 0.92F, 0.80F); // an open menu's item
	colors[ImGuiCol_HeaderHovered] = ImVec4(0.04F, 0.40F, 0.92F, 0.90F);
	colors[ImGuiCol_HeaderActive] = ImVec4(0.04F, 0.40F, 0.92F, 1.0F);
	imguiIO.FontDefault = defaultFont;

	// we allow up to queue count in-flight renders per frame due to triple buffering. passed in rather than read here,
	// since the caller already holds the graphics queue write lock.
	myRenderer = std::make_unique<ImGuiRenderer>(rhi.GetPrimaryDevice(), swapchain, graphicsQueue, graphicsQueueCount);
	platform::imgui::Init(window);

	// IMNODES_NAMESPACE::CreateContext();
	// IMNODES_NAMESPACE::LoadCurrentEditorStateFromIniString(
	//	myNodeGraph.layout.c_str(), myNodeGraph.layout.size());
}

ImGuiLayer::~ImGuiLayer()
{
	// size_t count;
	// myNodeGraph.layout.assign(IMNODES_NAMESPACE::SaveCurrentEditorStateToIniString(&count));
	// IMNODES_NAMESPACE::DestroyContext();

	myRenderer.reset();
	platform::imgui::Shutdown();

	// snapshot draw lists are registered with the context's shared data, and must be gone before it is destroyed
	for (auto& frame : myFrames)
	{
		frame.drawData.Clear();
		frame.snapshot.Clear();
	}

	ImGui::DestroyContext();
}

void ImGuiLayer::BeginFrame()
{
	platform::imgui::NewFrame(); // polls the window's input into imgui's io
	myRenderer->NewFrame();
	ImGui::NewFrame();
}

void ImGuiLayer::EndFrame()
{
	ImGui::Render();

	// process texture creates/updates/destroys here rather than in the render thread: the snapshot below
	// outlives this frame, and imgui frees ImTextureData (e.g. when the font atlas grows) on the next NewFrame().
	myRenderer->UpdateTextures(++myFrameSequence);

	if (auto *data = ImGui::GetDrawData())
	{
		auto& [snapshot, drawData, sequence, blurRects] = myFrames[myWriteFrame];
		snapshot.SnapUsingSwap(data, &drawData, ImGui::GetTime());
		sequence = myFrameSequence;

		// the frosted panels, as macOS's: the main menu bar's window (BeginMainMenuBar's), and the menus, popups and
		// tooltips open (not modals), in pixels of the framebuffer, with their windows' corners
		blurRects.clear();
		const auto& io = ImGui::GetIO();
		for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
		{
			if (!window->Active || window->Hidden)
				continue;
			bool frosted = (window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Tooltip)) != 0 &&
						   (window->Flags & ImGuiWindowFlags_Modal) == 0;
			frosted = frosted || std::strcmp(window->Name, "##MainMenuBar") == 0;
			if (!frosted)
				continue;
			UiBlurRect rect{};
			rect.rect[0] = window->Pos.x * io.DisplayFramebufferScale.x;
			rect.rect[1] = window->Pos.y * io.DisplayFramebufferScale.y;
			rect.rect[2] = (window->Pos.x + window->Size.x) * io.DisplayFramebufferScale.x;
			rect.rect[3] = (window->Pos.y + window->Size.y) * io.DisplayFramebufferScale.y;
			rect.radius = window->WindowRounding * io.DisplayFramebufferScale.y;
			blurRects.push_back(rect);
		}

		// detach the snapshot from imgui-owned texture data: resolve texture refs to their (already uploaded)
		// backend ids, and drop the texture list so RenderDrawData in the render thread doesn't touch it.
		for (ImDrawList* drawList : drawData.CmdLists)
			for (ImDrawCmd& drawCmd : drawList->CmdBuffer)
				drawCmd.TexRef = ImTextureRef(drawCmd.GetTexID());
		drawData.Textures = nullptr;

		// publish, and continue with the previous pending frame (whether or not the draw thread took it)
		myWriteFrame = myPendingFrame.exchange(myWriteFrame | kFrameFresh, std::memory_order_acq_rel) & ~kFrameFresh;
	}
}

std::span<const UiBlurRect> ImGuiLayer::TakeFrame()
{
	// before the texture uploads (PrepareFrame): a frame is published after the uploads it depends on were queued. keep
	// drawing the previous frame if no new one was published.
	if ((myPendingFrame.load(std::memory_order_relaxed) & kFrameFresh) != 0)
		myReadFrame = myPendingFrame.exchange(myReadFrame, std::memory_order_acq_rel) & ~kFrameFresh;

	return myFrames[myReadFrame].blurRects;
}

void ImGuiLayer::PrepareFrame(CommandBufferHandle cmd, std::vector<core::TaskHandle>& callbacks)
{
	ZoneScopedN("ImGuiLayer::PrepareFrame");

	myRenderer->PrepareFrame(cmd, myFrames[myReadFrame].sequence, callbacks);
}

ImTextureID ImGuiLayer::AddTexture(ImageViewHandle view)
{
	return static_cast<ImTextureID>(myRenderer->AddTexture(view));
}

void ImGuiLayer::RemoveTexture(ImTextureID id)
{
	// the frame being built is published as the next sequence, and doesn't draw it
	myRenderer->RemoveTexture(static_cast<uint64_t>(id), myFrameSequence + 1);
}

void ImGuiLayer::Draw(CommandBufferHandle cmd)
{
	ZoneScopedN("ImGuiLayer::Draw");

	myRenderer->Render(myFrames[myReadFrame].drawData, cmd);
}

} // namespace gfx
