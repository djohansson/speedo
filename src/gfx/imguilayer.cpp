#include "imguilayer.h"

#include <core/application.h>
#include <core/profiling.h>
#include <platform/imguiplatform.h>

#include <algorithm>
#include <array>
#include <filesystem>

namespace gfx
{

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

	GetStyle().ScaleAllSizes(std::max(dpiScaleX, dpiScaleY));

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.RasterizerDensity = std::max(window.GetState().xscale, window.GetState().yscale);
	config.PixelSnapH = false;

	imguiIO.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	std::filesystem::path fontPath(std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]));
	fontPath /= "fonts";
	fontPath /= "foo";

	constexpr std::array<const char*, 6> kFonts{{
		"Cousine-Regular.ttf",
		"DroidSans.ttf",
		"Karla-Regular.ttf",
		"ProggyClean.ttf",
		"ProggyTiny.ttf",
		"Roboto-Medium.ttf",
	}};

	constexpr float kDefaultFontSize = 16.0F;
	ImFont* defaultFont = nullptr;
	for (const auto* font : kFonts)
	{
		fontPath.replace_filename(font);
		defaultFont = imguiIO.Fonts->AddFontFromFileTTF(
			fontPath.generic_string().c_str(), kDefaultFontSize * std::max(dpiScaleX, dpiScaleY), &config);
	}

	// Setup style
	StyleColorsClassic();
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
		auto& [snapshot, drawData, sequence] = myFrames[myWriteFrame];
		snapshot.SnapUsingSwap(data, &drawData, ImGui::GetTime());
		sequence = myFrameSequence;

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

void ImGuiLayer::PrepareFrame(CommandBufferHandle cmd, std::vector<core::TaskHandle>& callbacks)
{
	ZoneScopedN("ImGuiLayer::PrepareFrame");

	// take the frame before the texture uploads: a frame is published after the uploads it depends on were queued.
	// keep drawing the previous frame if no new one was published.
	if ((myPendingFrame.load(std::memory_order_relaxed) & kFrameFresh) != 0)
		myReadFrame = myPendingFrame.exchange(myReadFrame, std::memory_order_acq_rel) & ~kFrameFresh;

	myRenderer->PrepareFrame(cmd, myFrames[myReadFrame].sequence, callbacks);
}

void ImGuiLayer::Draw(CommandBufferHandle cmd)
{
	ZoneScopedN("ImGuiLayer::Draw");

	myRenderer->Render(myFrames[myReadFrame].drawData, cmd);
}

} // namespace gfx
