#include "windowedapplicationdetail.h"

#include <gfx/lightgizmos.h>

#include <version.h>

#include <imgui.h>

#include <glm/mat4x4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <tuple>
#include <numbers>

// a menu item's shortcut label, by the platform's name for ImGuiMod_Ctrl (cmd on macOS, see PrepareDraw)
#if defined(__APPLE__)
#	define SHORTCUT_LABEL(key) "Cmd+" key
#else
#	define SHORTCUT_LABEL(key) "Ctrl+" key
#endif

namespace client
{

namespace ui
{

// SPEEDO_OPEN_MENUS=<label>[/<label>...]: the menus of these labels are held open (each where its parent menu is), to
// show them in screenshots without clicking (which would bring the window to the front)
[[nodiscard]] bool BeginMenu(const char* label)
{
	static const std::string kOpenMenus = []
	{
		const char* menus = std::getenv("SPEEDO_OPEN_MENUS");
		return menus != nullptr ? std::format("/{}/", menus) : std::string();
	}();
	if (!kOpenMenus.empty() && kOpenMenus.find(std::format("/{}/", label)) != std::string::npos)
		ImGui::OpenPopup(ImGui::GetID(label));
	return ImGui::BeginMenu(label);
}

} // namespace ui

void WindowedApplication::PrepareDraw()
{
	ZoneScopedN("WindowedApplication::PrepareDraw");

	using namespace windowedapplication;
	using namespace ImGui;

	auto& rhi = GetRHI();
	auto& device = rhi.GetPrimaryDevice();

	myImGui->BeginFrame();

	// ticks per second: PrepareDraw runs once per tick, measured like the frame rate (see Draw)
	static float gTicksPerSecond = 0.0F;
	{
		using namespace std::chrono_literals;
		static auto gTpsTime = std::chrono::steady_clock::now();
		static uint32_t gTpsTickCount = 0;
		gTpsTickCount++;
		if (auto now = std::chrono::steady_clock::now(); now - gTpsTime >= 500ms)
		{
			gTicksPerSecond = static_cast<float>(gTpsTickCount) / std::chrono::duration<float>(now - gTpsTime).count();
			gTpsTickCount = 0;
			gTpsTime = now;
		}
	}

	// frame/tick rate overlay in the top right corner, below the menu bar
	if (gShowFps || gShowTps)
	{
		constexpr float kFpsOverlayPadding = 10.0F;
		constexpr float kFpsOverlayBgAlpha = 0.35F;
		const auto* viewport = GetMainViewport();
		SetNextWindowPos(
			ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - kFpsOverlayPadding, viewport->WorkPos.y + kFpsOverlayPadding),
			ImGuiCond_Always,
			ImVec2(1.0F, 0.0F));
		SetNextWindowBgAlpha(kFpsOverlayBgAlpha);
		if (Begin(
				"Rates",
				nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
		{
			if (gShowFps)
			{
				float fps = gFramesPerSecond.load(std::memory_order_relaxed);
				Text("%.0f FPS (%.2f ms)", fps, fps > 0.0F ? 1000.0F / fps : 0.0F);
			}
			if (gShowTps)
				Text("%.0f TPS (%.2f ms)", gTicksPerSecond, gTicksPerSecond > 0.0F ? 1000.0F / gTicksPerSecond : 0.0F);
		}
		End();
	}

	// the move speed for a moment after it changes (with the mouse wheel, or a newly framed model), at the bottom
	{
		using namespace std::chrono_literals;
		static float gShownMoveSpeed = myViews->GetMoveSpeed();
		static auto gMoveSpeedChanged = std::chrono::steady_clock::time_point{};
		auto now = std::chrono::steady_clock::now();
		if (auto moveSpeed = myViews->GetMoveSpeed(); moveSpeed != gShownMoveSpeed)
		{
			gShownMoveSpeed = moveSpeed;
			gMoveSpeedChanged = now;
		}
		if (now - gMoveSpeedChanged < 1500ms)
		{
			constexpr float kOverlayPadding = 10.0F;
			constexpr float kOverlayBgAlpha = 0.35F;
			const auto* viewport = GetMainViewport();
			SetNextWindowPos(
				ImVec2(viewport->WorkPos.x + (0.5F * viewport->WorkSize.x), viewport->WorkPos.y + viewport->WorkSize.y - kOverlayPadding),
				ImGuiCond_Always,
				ImVec2(0.5F, 1.0F));
			SetNextWindowBgAlpha(kOverlayBgAlpha);
			if (Begin(
					"Move speed",
					nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
						ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
				Text("Move speed: %.3g units/s", gShownMoveSpeed);
			End();
		}
	}

	// one row per load in progress
	if (bool loading = !gLoads.Empty() &&
				 Begin(
					 "Loading",
					 &loading,
					 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration |
						 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
						 ImGuiWindowFlags_AlwaysAutoResize |
						 // appearing, it would take the focus (the browser's, whose previews load through here)
						 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
	{
		constexpr uint8_t kProgressMax = 255;
		constexpr float kProgressBarWidth = 160.0F;
		gLoads.ForEach([](const core::LoadQueue::Load& load)
		{
			ProgressBar((1.F / kProgressMax) * static_cast<float>(load.progress), ImVec2(kProgressBarWidth, 0));
			SameLine();
			TextUnformatted(load.name.c_str());
		});
		End();
	}

	static float gMenuBarHeight = 0.0F; // in imgui's coordinates, as the menu bar drew last frame
	// the ui fades out when it isn't in use: when the mouse isn't over the menu bar and no other window, menu or popup is
	// up, after a moment, and back in when it is (or one comes up, from a shortcut, a load's choice, ...). the overlays
	// above stay as they are. the windows are last frame's, the menu bar's height too
	{
		constexpr float kFadeInSeconds = 0.15F;
		constexpr float kFadeOutSeconds = 0.5F;
		constexpr float kHoldSeconds = 1.0F; // in use this recently counts as in use
		static float gFade = 1.0F; // 0: faded out, 1: shown
		static float gIdleSeconds = 0.0F;
		const auto& io = GetIO();
		bool overMenuBar = IsMousePosValid() && io.MousePos.y >= 0.0F && io.MousePos.y < gMenuBarHeight;
		bool inUse = overMenuBar || myImGui->ShowedFadingWindows() || IsAnyItemActive();
		gIdleSeconds = inUse ? 0.0F : gIdleSeconds + io.DeltaTime;
		gFade = gIdleSeconds < kHoldSeconds ? std::min(gFade + (io.DeltaTime / kFadeInSeconds), 1.0F)
											: std::max(gFade - (io.DeltaTime / kFadeOutSeconds), 0.0F);
		myImGui->Fade(gFade * gFade * (3.0F - (2.0F * gFade))); // eased
	}

	// View > Statistics (SPEEDO_SHOW_STATISTICS=1 at startup), in the builds that profile (debug and profile): the pass
	// timings (the frame graph's, written in those builds only), submits and transient memory, and in debug the vulkan
	// objects (tracked by validation builds only)
#if (SPEEDO_PROFILING_LEVEL > 0)
	static bool gShowStatistics = []
	{
		const char* show = std::getenv("SPEEDO_SHOW_STATISTICS");
		return show != nullptr && std::string_view(show) == "1";
	}();
	{
		if (gShowStatistics)
		{
			if (Begin("Statistics", &gShowStatistics, ImGuiWindowFlags_AlwaysAutoResize))
			{
				CloseOnEscape(gShowStatistics);
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
				for (const auto& [name, count] : GetObjectCounts<kGraphicsApi>())
					Text("%.*s: %u", static_cast<int>(name.size()), name.data(), count);
				Separator();
#endif

				// gpu submits per frame, averaged over half a second as the frame rate is (see Draw)
				Text("Queue Submits: %.1f/frame", gSubmitsPerFrame.load(std::memory_order_relaxed));
				Text("Submit Batches: %.1f/frame", gSubmitBatchesPerFrame.load(std::memory_order_relaxed));

				// the frame graph's passes, on the gpu (see GpuProfiler::Collect): only where the backend writes their timestamps
				// (KosmicKrisp: at the beginning and end of its own command encoders)
				Separator();
				if (myScene)
					for (const auto& [name, milliseconds] : myScene->GetRenderer().GetPassTimings())
						Text("%s: %.3f ms", name.c_str(), milliseconds);

				// the frame graph's transients (see FrameGraph), in shared memory
				Separator();
				constexpr double kMiB = 1024.0 * 1024.0;
				Text(
					"Transient Memory: %.1f MiB (%.1f MiB unaliased)",
					static_cast<double>(App().GetScene().GetTransientMemory()) / kMiB,
					static_cast<double>(App().GetScene().GetUnaliasedTransientMemory()) / kMiB);
			}
			End();
		}
	}
#endif

	// the scene's lights (View > Lights): listed, and drawn over the views (see gfx::BuildLightGizmos)
	static bool gShowLights = []
	{
		const char* show = std::getenv("SPEEDO_SHOW_LIGHTS");
		return show != nullptr && std::string_view(show) == "1";
	}();
	App().GetScene().GetSettings().captureLights.store(gShowLights, std::memory_order_relaxed);
	if (gShowLights)
	{
		static bool gLightMarkers = true;
		static bool gShadowFrusta = true;
		static std::optional<size_t> gSelectedLight;
		auto capture = App().GetScene().GetRenderer().GetLightCapture();
		if (gSelectedLight && *gSelectedLight >= capture.lights.size())
			gSelectedLight.reset();

		if (Begin("Lights", &gShowLights, ImGuiWindowFlags_AlwaysAutoResize))
		{
			CloseOnEscape(gShowLights);
			Checkbox("Markers", &gLightMarkers);
			SameLine();
			Checkbox("Shadow Frusta", &gShadowFrusta);
			SetItemTooltip("Each shadow view's frustum: a directional light's cascades, a point light's cube faces, a spot light's view.");
			TextDisabled("The model's lights, then the environment's dominant lights (merged ones aren't drawn).");
			if (Selectable("All", !gSelectedLight))
				gSelectedLight.reset();
			for (size_t lightIt = 0; lightIt < capture.lights.size(); lightIt++)
			{
				const auto& light = capture.lights[lightIt];
				static constexpr std::array kTypeNames{"Directional", "Point", "Spot"};
				const char* type = light.type < kTypeNames.size() ? kTypeNames[light.type] : "?";
				float luminance = (0.2126F * light.intensity[0]) + (0.7152F * light.intensity[1]) + (0.0722F * light.intensity[2]);
				uint32_t first = lightIt < capture.lightShadows.size() ? capture.lightShadows[lightIt] : SHADER_TYPES_NO_SHADOW;
				auto label = std::format(
					"#{} {} ({}): {:.3g} {}, {}",
					lightIt,
					type,
					lightIt < capture.modelLightCount ? "model" : "environment",
					luminance,
					light.type == LIGHT_TYPE_DIRECTIONAL ? "lux" : "cd",
					first == SHADER_TYPES_NO_SHADOW ? std::string("no shadows") : std::format("{} shadow views", gfx::ShadowViewCount(light.type)));
				PushID(static_cast<int>(lightIt));
				float brightest = std::max({light.intensity[0], light.intensity[1], light.intensity[2], 1e-6F});
				ColorButton(
					"##color",
					ImVec4(light.intensity[0] / brightest, light.intensity[1] / brightest, light.intensity[2] / brightest, 1.0F),
					ImGuiColorEditFlags_NoTooltip,
					ImVec2(GetTextLineHeight(), GetTextLineHeight()));
				SameLine();
				if (Selectable(label.c_str(), gSelectedLight == lightIt))
					gSelectedLight = lightIt;
				PopID();
			}
		}
		End();

		// over each view: its camera's projection into its viewport (framebuffer pixels, imgui's coordinates are points)
		auto gizmos = gfx::BuildLightGizmos(capture, {.shadowFrusta = gShadowFrusta, .light = gSelectedLight});
		auto* drawList = GetBackgroundDrawList();
		ImVec2 scale = GetIO().DisplayFramebufferScale;
		auto viewports = myViews->GetViewports();
		for (size_t viewIt = 0; viewIt < viewports.size(); viewIt++)
		{
			auto camera = myViews->GetCamera(viewIt);
			if (!camera)
				continue;
			const auto& viewport = viewports[viewIt];
			glm::mat4 viewProjection = camera->GetProjectionMatrix() * camera->GetViewMatrix();
			auto toScreen = [&viewport, scale](glm::vec4 clip)
			{
				glm::vec2 ndc = glm::vec2(clip) / clip.w;
				return ImVec2(
					(static_cast<float>(viewport.x) + ((ndc.x * 0.5F) + 0.5F) * static_cast<float>(viewport.width)) / scale.x,
					(static_cast<float>(viewport.y) + ((ndc.y * 0.5F) + 0.5F) * static_cast<float>(viewport.height)) / scale.y);
			};
			constexpr float kNearW = 1e-3F; // in front of the camera
			drawList->PushClipRect(
				ImVec2(static_cast<float>(viewport.x) / scale.x, static_cast<float>(viewport.y) / scale.y),
				ImVec2(static_cast<float>(viewport.x + viewport.width) / scale.x, static_cast<float>(viewport.y + viewport.height) / scale.y),
				true);
			for (const auto& line : gizmos.lines)
			{
				glm::vec4 from = viewProjection * glm::vec4(line.from, 1.0F);
				glm::vec4 to = viewProjection * glm::vec4(line.to, 1.0F);
				if (from.w < kNearW && to.w < kNearW)
					continue;
				// clipped to what is in front of the camera
				if (from.w < kNearW)
					from += (to - from) * ((kNearW - from.w) / (to.w - from.w));
				else if (to.w < kNearW)
					to += (from - to) * ((kNearW - to.w) / (from.w - to.w));
				drawList->AddLine(
					toScreen(from), toScreen(to), ColorConvertFloat4ToU32(ImVec4(line.color.x, line.color.y, line.color.z, line.color.w)), 1.5F);
			}
			if (gLightMarkers)
				for (const auto& marker : gizmos.markers)
				{
					// a directional light where it is in the sky (at infinity), kept at the view's edge when it is out of
					// it, dimmed if behind the camera
					bool directional = marker.type == LIGHT_TYPE_DIRECTIONAL;
					glm::vec4 clip = viewProjection * glm::vec4(marker.position, directional ? 0.0F : 1.0F);
					float alpha = 1.0F;
					if (directional)
					{
						glm::vec2 ndc = glm::vec2(clip) / std::max(std::abs(clip.w), kNearW);
						if (clip.w < kNearW)
						{
							ndc = -glm::vec2(clip);
							alpha = 0.4F;
						}
						float extent = std::max({std::abs(ndc.x), std::abs(ndc.y), 1e-6F});
						constexpr float kEdge = 0.92F;
						if (clip.w < kNearW || extent > kEdge)
							ndc *= kEdge / extent;
						clip = glm::vec4(ndc, 0.0F, 1.0F);
					}
					else if (clip.w < kNearW)
						continue;
					ImVec2 at = toScreen(clip);
					ImU32 color = ColorConvertFloat4ToU32(ImVec4(marker.color.x, marker.color.y, marker.color.z, alpha));
					constexpr ImU32 kOutline = IM_COL32(0, 0, 0, 220);
					constexpr float kRadius = 7.0F;
					// a sun with rays, a bulb, or a spot's cone
					if (marker.type == LIGHT_TYPE_DIRECTIONAL)
						for (int rayIt = 0; rayIt < 8; rayIt++)
						{
							float angle = static_cast<float>(rayIt) * std::numbers::pi_v<float> / 4.0F;
							ImVec2 direction(std::cos(angle), std::sin(angle));
							drawList->AddLine(
								ImVec2(at.x + direction.x * kRadius * 1.4F, at.y + direction.y * kRadius * 1.4F),
								ImVec2(at.x + direction.x * kRadius * 2.1F, at.y + direction.y * kRadius * 2.1F),
								color,
								2.0F);
						}
					else if (marker.type == LIGHT_TYPE_SPOT)
						drawList->AddTriangleFilled(
							ImVec2(at.x, at.y + kRadius * 0.4F), ImVec2(at.x - kRadius * 1.3F, at.y + kRadius * 2.2F),
							ImVec2(at.x + kRadius * 1.3F, at.y + kRadius * 2.2F), color);
					drawList->AddCircleFilled(at, kRadius, color);
					drawList->AddCircle(at, kRadius, kOutline, 0, 1.5F);
					auto label = std::format("#{}", marker.light);
					drawList->AddText(ImVec2(at.x + kRadius * 2.3F, at.y - kRadius), IM_COL32(255, 255, 255, 230), label.c_str());
				}
			drawList->PopClipRect();
		}
	}

	InternalDrawBrowser();

	// the models of an archive with several (see LoadAndInstallArchive)
	static std::optional<ArchiveChoice> gShownArchiveChoice;
	if (!gShownArchiveChoice)
	{
		std::scoped_lock lock(gArchiveChoiceMutex);
		if (gArchiveChoice)
		{
			gShownArchiveChoice = std::move(gArchiveChoice);
			gArchiveChoice.reset();
			OpenPopup("Load Model");
		}
	}
	if (BeginPopupModal("Load Model", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		const auto& choice = *gShownArchiveChoice;
		Text("%s holds %zu models:", choice.archive.c_str(), choice.models.size());

		constexpr float kListHeight = 300.0F;
		std::optional<std::filesystem::path> chosen;
		if (BeginChild("Models", ImVec2(0.0F, kListHeight), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_Borders))
		{
			for (const auto& model : choice.models)
				if (Selectable(model.lexically_relative(choice.directory).string().c_str()))
					chosen = model;
		}
		EndChild();

		if (chosen)
			(void)gLoads.Enqueue(
				chosen->filename().string(),
				[&rhi, path = chosen->string()](std::atomic_uint8_t& progress) { LoadAndInstallModel(rhi, path, progress); });

		bool all = Button("All, side by side");
		if (all)
		{
			std::vector<std::string> paths;
			for (const auto& model : choice.models)
				paths.push_back(model.string());
			(void)gLoads.Enqueue(
				choice.archive,
				[&rhi, paths = std::move(paths)](std::atomic_uint8_t& progress) { App().GetScene().LoadModels(paths, progress); });
		}
		SameLine();

		// escape cancels too (see CloseOnEscape)
		if (chosen || all || Button("Cancel") || Shortcut(ImGuiKey_Escape))
		{
			gShownArchiveChoice.reset();
			CloseCurrentPopup();
		}
		EndPopup();
	}

	if (gShowDemoWindow)
		ShowDemoWindow(&gShowDemoWindow);

	if (gShowAbout)
	{
		if (Begin("About client", &gShowAbout, ImGuiWindowFlags_AlwaysAutoResize))
		{
			CloseOnEscape(gShowAbout);
			Text("speedo %d.%d.%d", speedo::version::Major, speedo::version::Minor, speedo::version::Patch);
			TextDisabled("A real-time renderer and asset viewer, built on Vulkan.");
			Separator();
			auto name = device.GetName();
			Text("GPU: %.*s", static_cast<int>(name.size()), name.data());
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
			Text("Build: debug (validation on)");
#elif (SPEEDO_PROFILING_LEVEL > 0)
			Text("Build: profile");
#else
			Text("Build: release");
#endif
		}
		End();
	}

	auto resourcePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]);


	// automation: SPEEDO_AUTOLOAD_MODEL (a model, or a zip archive or directory, whose models are loaded side by side)
	// and SPEEDO_AUTOLOAD_IMAGE (an image, on the default material) name files to load at startup, absolute or relative to
	// the resource directory, through the same load + install path as the "File" menu (SPEEDO_AUTOLOAD_SCENE=<index>: a
	// gltf model's scene, as View > Scene loads it). with
	// SPEEDO_AUTOLOAD_EXIT=<frames>, the application exits that many frames after the loads have finished (see
	// scripts/assettest.ps1).
	static std::vector<core::Future<void>> gAutoLoads;
	static std::optional<uint32_t> gAutoLoadExitFrames;
	// SPEEDO_CAMERA=<x>,<y>,<z>,<pitch>,<yaw> (world space, degrees): where the views look from once the autoloads are
	// installed (the model's install frames them first), for measurements and screenshots; SPEEDO_CAMERA=frame frames the
	// model, as View > Camera > Frame model (a model with cameras starts looking through its first)
	static std::optional<std::array<float, 5>> gAutoCamera;
	static bool gAutoFrame = false;
	static bool gAutoCameraAfterModel = false; // wait for the model's install (on the draw thread), which frames the views
	if (static bool gAutoLoadDone = false; !gAutoLoadDone)
	{
		gAutoLoadDone = true;

		// queued as separate loads, which run concurrently
		// a zip archive loads all of its models, side by side
		mesh::ImportOptions autoLoadOptions;
		if (const char* scene = std::getenv("SPEEDO_AUTOLOAD_SCENE"); scene != nullptr && *scene != '\0')
			autoLoadOptions.scene = std::strtoull(scene, nullptr, 10);
		if (const char* variant = std::getenv("SPEEDO_AUTOLOAD_VARIANT"); variant != nullptr && *variant != '\0')
			autoLoadOptions.variant = std::strtoull(variant, nullptr, 10);
		if (const char* autoLoadModel = std::getenv("SPEEDO_AUTOLOAD_MODEL"); autoLoadModel != nullptr && *autoLoadModel != '\0')
			gAutoLoads.emplace_back(gLoads.Enqueue(
				autoLoadModel,
				[&rhi, path = (resourcePath / autoLoadModel).string(), autoLoadOptions](std::atomic_uint8_t& progress)
				{
					if ((autoLoadOptions.scene || autoLoadOptions.variant) && mesh::IsModelFile(path))
						App().GetScene().LoadModels({path}, progress, autoLoadOptions);
					else
						LoadAndInstallFile(rhi, path, progress, ArchiveModels::kAll);
				}));
		// the tonemapper (SPEEDO_TONEMAPPER: pbr-neutral, aces, agx, reinhard or linear) and auto exposure
		// (SPEEDO_AUTO_EXPOSURE=1) to start with (as View sets them)
		if (const char* tonemapper = std::getenv("SPEEDO_TONEMAPPER"); tonemapper != nullptr)
		{
			std::string_view name(tonemapper);
			App().GetScene().GetSettings().tonemapper.store(
				name == "aces"		 ? TONEMAPPER_ACES
				: name == "agx"		 ? TONEMAPPER_AGX
				: name == "reinhard" ? TONEMAPPER_REINHARD
				: name == "linear"	 ? TONEMAPPER_LINEAR
									 : TONEMAPPER_PBR_NEUTRAL,
				std::memory_order_relaxed);
		}
		if (const char* autoExposure = std::getenv("SPEEDO_AUTO_EXPOSURE"); autoExposure != nullptr && std::string_view(autoExposure) == "1")
			App().GetScene().GetSettings().autoExposure.store(true, std::memory_order_relaxed);
		if (const char* shadows = std::getenv("SPEEDO_SHADOWS"); shadows != nullptr && std::string_view(shadows) == "0")
			App().GetScene().GetSettings().shadows.store(false, std::memory_order_relaxed);
		if (const char* culling = std::getenv("SPEEDO_CULLING"); culling != nullptr && std::string_view(culling) == "0")
			App().GetScene().GetSettings().culling.store(false, std::memory_order_relaxed);
		if (const char* clustered = std::getenv("SPEEDO_CLUSTERED_LIGHTS"); clustered != nullptr && std::string_view(clustered) == "0")
			App().GetScene().GetSettings().clusteredLights.store(false, std::memory_order_relaxed);
		if (const char* merge = std::getenv("SPEEDO_MERGE_LIGHTS"); merge != nullptr && std::string_view(merge) == "0")
			App().GetScene().GetSettings().mergeLights.store(false, std::memory_order_relaxed);
		// the environment's rotation (degrees) and intensity to start with (as View > Environment sets them)
		if (const char* rotation = std::getenv("SPEEDO_ENVIRONMENT_ROTATION"); rotation != nullptr && *rotation != '\0')
			App().GetScene().GetSettings().environmentRotationDegrees.store(std::strtof(rotation, nullptr), std::memory_order_relaxed);
		if (const char* intensity = std::getenv("SPEEDO_ENVIRONMENT_INTENSITY"); intensity != nullptr && *intensity != '\0')
			App().GetScene().GetSettings().environmentIntensity.store(std::max(std::strtof(intensity, nullptr), 0.0F), std::memory_order_relaxed);
		// the environment: SPEEDO_AUTOLOAD_ENVIRONMENT's panorama, else the procedural sky
		std::optional<std::string> environmentPath;
		if (const char* autoLoadEnvironment = std::getenv("SPEEDO_AUTOLOAD_ENVIRONMENT");
			autoLoadEnvironment != nullptr && *autoLoadEnvironment != '\0')
			environmentPath = (resourcePath / autoLoadEnvironment).string();
		gAutoLoads.emplace_back(gLoads.Enqueue(
			environmentPath.value_or("Procedural sky"),
			[&rhi, environmentPath](std::atomic_uint8_t& progress) { App().GetScene().LoadEnvironment(environmentPath, progress); }));
		if (const char* autoLoadImage = std::getenv("SPEEDO_AUTOLOAD_IMAGE"); autoLoadImage != nullptr && *autoLoadImage != '\0')
			gAutoLoads.emplace_back(gLoads.Enqueue(
				autoLoadImage,
				[&rhi, path = (resourcePath / autoLoadImage).string()](std::atomic_uint8_t& progress)
				{ App().GetScene().LoadImage(path, progress); }));
		if (const char* autoLoadExit = std::getenv("SPEEDO_AUTOLOAD_EXIT"); autoLoadExit != nullptr && *autoLoadExit != '\0')
			gAutoLoadExitFrames = static_cast<uint32_t>(std::strtoul(autoLoadExit, nullptr, 10));
		if (const char* camera = std::getenv("SPEEDO_CAMERA"); camera != nullptr && std::string_view(camera) == "frame")
		{
			gAutoFrame = true;
			const char* model = std::getenv("SPEEDO_AUTOLOAD_MODEL");
			gAutoCameraAfterModel = model != nullptr && *model != '\0';
		}
		else if (camera != nullptr && *camera != '\0')
		{
			std::array<float, 5> values{};
			const char* it = camera;
			size_t count = 0;
			for (; count < values.size() && *it != '\0'; count++)
			{
				char* end = nullptr;
				values[count] = std::strtof(it, &end);
				if (end == it)
					break;
				it = *end == ',' ? end + 1 : end;
			}
			if (count == values.size())
			{
				gAutoCamera = values;
				const char* model = std::getenv("SPEEDO_AUTOLOAD_MODEL");
				gAutoCameraAfterModel = model != nullptr && *model != '\0';
			}
			else
				std::println(stderr, "SPEEDO_CAMERA: expected x,y,z,pitch,yaw, got {}", camera);
		}
	}

	if ((gAutoCamera || gAutoFrame) && std::ranges::all_of(gAutoLoads, [](const auto& load) { return load.IsReady(); }) &&
		(!gAutoCameraAfterModel || !App().GetScene().GetScenes().Read().Get().filePath.empty()))
	{
		if (gAutoFrame)
		{
			// on the draw thread, as the Camera menu does
			auto [task, future] = core::CreateTask([this] { myViews->UseSceneCamera(std::nullopt); });
			rhi.drawCalls.enqueue(task);
		}
		else
		{
			const auto& values = *gAutoCamera;
			myViews->Place(glm::vec3(values[0], values[1], values[2]), glm::vec3(glm::radians(values[3]), glm::radians(values[4]), 0.0F));
		}
		gAutoCamera.reset();
		gAutoFrame = false;
	}

	// the installs are queued to the draw thread when the loads finish, so they have happened a frame later
	if (gAutoLoadExitFrames && std::ranges::all_of(gAutoLoads, [](const auto& load) { return load.IsReady(); }))
		if ((*gAutoLoadExitFrames)-- == 0)
			core::Application::Get()->RequestExit();

	// the views' grid is owned by the draw thread (see Views::OnResizeGrid), so track the requested one here. nothing else
	// changes the grid, so the window's is current when this is initialized
	static Extent2d gSplitScreenGrid{.width = myViews->GetGrid().x, .height = myViews->GetGrid().y};
	auto setSplitScreenGrid = [this, &rhi](uint32_t width, uint32_t height)
	{
		if (gSplitScreenGrid.width == width && gSplitScreenGrid.height == height)
			return;
		gSplitScreenGrid = {.width = width, .height = height};
		auto [resizeTask, resizeFuture] = core::CreateTask(
			[this, grid = gSplitScreenGrid] { myViews->OnResizeGrid({grid.width, grid.height}); });
		rhi.drawCalls.enqueue(resizeTask);
	};

	// the menus' shortcuts: the labels MenuItem shows are only text. global routes, so they work whichever window has
	// the focus, unless its active item takes the chord (a text field's ctrl+a). ImGuiMod_Ctrl is cmd on macOS
	// (io.ConfigMacOSXBehaviors swaps them), as the labels say
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal))
	{
		gShowBrowser = !(gShowBrowser && gBrowserPurpose == BrowserPurpose::kFiles);
		gBrowserPurpose = BrowserPurpose::kFiles;
	}
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_E, ImGuiInputFlags_RouteGlobal))
	{
		gShowBrowser = !(gShowBrowser && gBrowserPurpose == BrowserPurpose::kEnvironment);
		gBrowserPurpose = BrowserPurpose::kEnvironment;
	}
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
		core::Application::Get()->RequestExit();
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_1, ImGuiInputFlags_RouteGlobal))
		setSplitScreenGrid(1, 1);
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_2, ImGuiInputFlags_RouteGlobal))
		setSplitScreenGrid(1, 2);
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_3, ImGuiInputFlags_RouteGlobal))
		setSplitScreenGrid(2, 1);
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_4, ImGuiInputFlags_RouteGlobal))
		setSplitScreenGrid(2, 2);

	// the menu bar, as macOS's: translucent over its backdrop, which the renderer blurs (a frosted panel, see
	// ImGuiLayer::TakeFrame), a little taller, without a border, with a faint edge below. its window would draw its own
	// background under the menu bar's (WindowBg, 85% opaque)
	PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
	PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(GetStyle().FramePadding.x, GetStyle().FramePadding.y * 1.75F));
	PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
	if (BeginMainMenuBar())
	{
		gMenuBarHeight = GetWindowHeight();
		auto* drawList = GetWindowDrawList();
		drawList->PushClipRectFullScreen();
		drawList->AddLine(
			ImVec2(GetWindowPos().x, GetWindowPos().y + GetWindowHeight() - 0.5F),
			ImVec2(GetWindowPos().x + GetWindowWidth(), GetWindowPos().y + GetWindowHeight() - 0.5F),
			IM_COL32(255, 255, 255, 40));
		drawList->PopClipRect();

		if (ui::BeginMenu("File"))
		{
			if (MenuItem("Open...", SHORTCUT_LABEL("O"), gShowBrowser && gBrowserPurpose == BrowserPurpose::kFiles))
			{
				gShowBrowser = !(gShowBrowser && gBrowserPurpose == BrowserPurpose::kFiles);
				gBrowserPurpose = BrowserPurpose::kFiles;
			}
			SetItemTooltip("Opens models, zip archives, images and panoramas from the file browser: preview them, and open one or several (side by side, or as authored).");
			Separator();
			if (MenuItem("Exit", SHORTCUT_LABEL("Q")))
				core::Application::Get()->RequestExit();

			ImGui::EndMenu();
		}
		if (ui::BeginMenu("View"))
		{
			// if (MenuItem("Node Editor..."))
			// 	showNodeEditor = !showNodeEditor;
			if (ui::BeginMenu("Layout"))
			{
				for (auto [label, shortcut, width, height] : std::array{
						 std::tuple{"1x1", SHORTCUT_LABEL("1"), 1U, 1U},
						 std::tuple{"1x2", SHORTCUT_LABEL("2"), 1U, 2U},
						 std::tuple{"2x1", SHORTCUT_LABEL("3"), 2U, 1U},
						 std::tuple{"2x2", SHORTCUT_LABEL("4"), 2U, 2U}})
					if (MenuItem(label, shortcut, gSplitScreenGrid.width == width && gSplitScreenGrid.height == height))
						setSplitScreenGrid(width, height);

				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Presentation"))
			{
				// how the window goes fullscreen (alt+enter) and presents (see FULLSCREEN_MODE_* and Swapchain::Configure):
				// to compare them, e.g. a driver's direct scanout on macOS
				auto& windowState = GetWindow(GetCurrentWindow()).GetState();
				SeparatorText("Fullscreen (Alt+Enter)");
				for (auto [label, mode] : std::array{
						 std::pair{"Exclusive", FULLSCREEN_MODE_EXCLUSIVE},
						 std::pair{"Borderless", FULLSCREEN_MODE_BORDERLESS},
#if defined(__APPLE__)
						 std::pair{"Native", FULLSCREEN_MODE_NATIVE},
#endif
					 })
					if (MenuItem(label, nullptr, windowState.fullscreenMode == mode))
						windowState.fullscreenMode = mode;
				SetItemTooltip("Exclusive takes the monitor (on macOS, native too: scanned out directly); borderless is a window covering it, which stays composited. Takes effect the next time the window goes fullscreen.");
				// what the swapchain has, of what the surface supports (none listed until the first frame)
				auto presentation = UnpackPresentation(myPresentation.load(std::memory_order_relaxed));
				SeparatorText("Swapchain");
				for (auto [label, option, tooltip] : std::array{
						 std::tuple{"FIFO", rhi::PresentationMode::kFifo, "In order, one frame per vertical blank."},
						 std::tuple{"FIFO Relaxed", rhi::PresentationMode::kFifoRelaxed, "As FIFO, but a late frame is shown at once (may tear then)."},
						 std::tuple{"Mailbox", rhi::PresentationMode::kMailbox, "One frame per vertical blank, the newest: no tearing, lowest latency."},
						 std::tuple{"Immediate", rhi::PresentationMode::kImmediate, "At once, without waiting for the vertical blank (may tear)."}})
				{
					if ((presentation.modes & (1U << static_cast<uint32_t>(option))) == 0)
						continue;
					if (MenuItem(label, nullptr, presentation.mode == option))
					{
						auto request = presentation;
						request.mode = option;
						myPresentationRequest.store(PackPresentation(request), std::memory_order_relaxed);
					}
					SetItemTooltip("%s", tooltip);
				}
				// the image counts the surface takes (no limit: two more than the least)
				auto maxImageCount = presentation.maxImageCount != 0 ? presentation.maxImageCount : presentation.minImageCount + 2;
				for (uint32_t count = presentation.minImageCount; count != 0 && count <= maxImageCount; count++)
				{
					bool only = presentation.minImageCount == maxImageCount;
					if (MenuItem(std::format("{} images{}", count, only ? " (all the surface takes)" : "").c_str(), nullptr, presentation.imageCount == count, !only))
					{
						auto request = presentation;
						request.imageCount = count;
						myPresentationRequest.store(PackPresentation(request), std::memory_order_relaxed);
					}
				}
				SetItemTooltip("The swapchain is recreated before the next frame.");
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Environment"))
			{
				// what lights the scene (see InstallEnvironment): the procedural sky, or a panorama file
				std::string current = App().GetScene().GetEnvironmentName().Read().Get();
				TextDisabled("%s", current.empty() ? "None" : current.c_str());
				{
					float intensity = App().GetScene().GetSettings().environmentIntensity.load(std::memory_order_relaxed);
					if (DragFloat("Intensity", &intensity, 0.01F, 0.0F, 100.0F, "%.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
						App().GetScene().GetSettings().environmentIntensity.store(intensity, std::memory_order_relaxed);
					SetItemTooltip("Scales its light. Drag, or double-click to type.");
					float rotation = App().GetScene().GetSettings().environmentRotationDegrees.load(std::memory_order_relaxed);
					if (DragFloat("Rotation", &rotation, 0.5F, -180.0F, 180.0F, "%.1f deg", ImGuiSliderFlags_WrapAround))
						App().GetScene().GetSettings().environmentRotationDegrees.store(rotation, std::memory_order_relaxed);
					SetItemTooltip("Turns it about the vertical axis. Drag, or double-click to type.");
					bool backdrop = App().GetScene().GetSettings().environmentBackdrop.load(std::memory_order_relaxed);
					if (MenuItem("Backdrop", nullptr, &backdrop))
						App().GetScene().GetSettings().environmentBackdrop.store(backdrop, std::memory_order_relaxed);
					// its strongest light sources, directional lights (with shadows) rather than part of the environment
					{
						auto info = App().GetScene().GetEnvironmentLightInfo().Read();
						if (info.Get().empty())
							TextDisabled("Dominant lights: none");
						for (size_t lightIt = 0; lightIt < info.Get().size(); lightIt++)
						{
							const auto& light = info.Get()[lightIt];
							if (light.merged)
								TextDisabled("Dominant light %zu: %.3g lux, the model's own", lightIt + 1, light.lux);
							else
								TextDisabled("Dominant light %zu: %.3g lux (x intensity)", lightIt + 1, light.lux);
							SetItemTooltip(
								"One of the environment's strongest light sources, which light the scene as directional "
								"lights (with shadows). Where the model has a directional light from the same direction, "
								"the model's is used instead, if merging is on.");
						}
					}
					bool merge = App().GetScene().GetSettings().mergeLights.load(std::memory_order_relaxed);
					if (MenuItem("Merge with the model's lights", nullptr, &merge))
						App().GetScene().GetSettings().mergeLights.store(merge, std::memory_order_relaxed);
					SetItemTooltip(
						"Treats a dominant light and a directional light of the model's within 5 degrees of it as one, "
						"the model's. Off: both light the scene.");
				}
				Separator();
				if (MenuItem("Procedural sky"))
					(void)gLoads.Enqueue(
						"Procedural sky",
						[&rhi](std::atomic_uint8_t& progress) { App().GetScene().LoadEnvironment(std::nullopt, progress); });
				if (MenuItem("Open Environment...", SHORTCUT_LABEL("E"), gShowBrowser && gBrowserPurpose == BrowserPurpose::kEnvironment))
				{
					// the file browser, listing panoramas only
					gShowBrowser = !(gShowBrowser && gBrowserPurpose == BrowserPurpose::kEnvironment);
					gBrowserPurpose = BrowserPurpose::kEnvironment;
				}
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Scene"))
			{
				// the installed gltf file's scenes: choosing one loads the file again with it
				Scene::SceneState scenes = App().GetScene().GetScenes().Read().Get();
				if (scenes.names.size() < 2)
					TextDisabled(scenes.names.empty() ? "No scenes" : "One scene");
				for (size_t sceneIt = 0; sceneIt < scenes.names.size(); sceneIt++)
				{
					PushID(static_cast<int>(sceneIt));
					if (MenuItem(scenes.names[sceneIt].c_str(), nullptr, scenes.current == sceneIt) && scenes.current != sceneIt)
						(void)gLoads.Enqueue(
							std::format("{} ({})", std::filesystem::path(scenes.filePath).filename().string(), scenes.names[sceneIt]),
							[&rhi, path = scenes.filePath, sceneIt, variant = scenes.variant](std::atomic_uint8_t& progress)
							{
								App().GetScene().LoadModels(
									{path}, progress, {.scene = sceneIt, .variant = variant ? std::optional<size_t>(*variant) : std::nullopt});
							});
					PopID();
				}
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Variant"))
			{
				// the installed gltf file's material variants (KHR_materials_variants): choosing one loads the file again
				// with its materials, in the scene shown
				Scene::SceneState scenes = App().GetScene().GetScenes().Read().Get();
				if (scenes.variants.empty())
					TextDisabled("No variants");
				auto load = [&scenes](std::optional<size_t> variant, std::string_view name)
				{
					(void)gLoads.Enqueue(
						std::format("{} ({})", std::filesystem::path(scenes.filePath).filename().string(), name),
						[path = scenes.filePath, scene = scenes.current, variant](std::atomic_uint8_t& progress)
						{ App().GetScene().LoadModels({path}, progress, {.scene = scene, .variant = variant}); });
				};
				if (!scenes.variants.empty() && MenuItem("Default", nullptr, !scenes.variant) && scenes.variant)
					load(std::nullopt, "default materials");
				for (size_t variantIt = 0; variantIt < scenes.variants.size(); variantIt++)
				{
					PushID(static_cast<int>(variantIt));
					bool current = scenes.variant == variantIt;
					if (MenuItem(scenes.variants[variantIt].c_str(), nullptr, current) && !current)
						load(variantIt, scenes.variants[variantIt]);
					PopID();
				}
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Animation"))
			{
				// the installed model's animations (see Model::Animate)
				auto animation = App().GetScene().GetAnimation().Write();
				auto& state = animation.Get();
				MenuItem("Play", nullptr, &state.playing);
				if (MenuItem("Restart"))
					state.time = 0.0;
				Separator();
				if (MenuItem("Rest pose", nullptr, !state.selected.has_value()))
					state.Select(std::nullopt);
				for (size_t animationIt = 0; animationIt < state.names.size(); animationIt++)
				{
					PushID(static_cast<int>(animationIt));
					if (MenuItem(state.names[animationIt].c_str(), nullptr, state.selected == animationIt))
						state.Select(animationIt);
					PopID();
				}
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Camera"))
			{
				// the installed model's cameras (see Views::SetScene), applied on the draw thread
				auto useCamera = [this, &rhi](std::optional<size_t> camera)
				{
					auto [task, future] = core::CreateTask([this, camera] { myViews->UseSceneCamera(camera); });
					rhi.drawCalls.enqueue(task);
				};
				auto current = myViews->GetSceneCamera();
				if (MenuItem("Frame model", nullptr, !current.has_value()))
					useCamera(std::nullopt);
				auto names = myViews->GetSceneCameraNames();
				if (!names.empty())
					Separator();
				for (size_t cameraIt = 0; cameraIt < names.size(); cameraIt++)
				{
					PushID(static_cast<int>(cameraIt));
					if (MenuItem(names[cameraIt].c_str(), nullptr, current == cameraIt))
						useCamera(cameraIt);
					PopID();
				}
				ImGui::EndMenu();
			}
			MenuItem("FPS", nullptr, &gShowFps);
			MenuItem("TPS", nullptr, &gShowTps);
			Separator();
			{
				// logarithmic: model scales span many orders of magnitude (see Views::FrameBounds). a drag box rather than a
				// slider: a double-click (or Ctrl/Cmd+click) types a number into it, which a slider only does on
				// Ctrl/Cmd+click. its speed is in the logarithmic range's terms: about 1% faster or slower per pixel.
				constexpr float kMinMoveSpeed = 1e-4F;
				constexpr float kMaxMoveSpeed = 1e6F;
				constexpr float kDragPixelsAcrossRange = 1000.0F;
				float moveSpeed = myViews->GetMoveSpeed();
				if (DragFloat(
						"Move speed",
						&moveSpeed,
						(kMaxMoveSpeed - kMinMoveSpeed) / kDragPixelsAcrossRange,
						kMinMoveSpeed,
						kMaxMoveSpeed,
						"%.3g units/s",
						ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
					myViews->SetMoveSpeed(moveSpeed);
				SetItemTooltip(
					"Drag to change, or double-click to type a speed.\n"
					"w, a, s, d move the camera under the mouse, and the mouse wheel changes the speed.");
			}
			{
				float stops = App().GetScene().GetSettings().exposureStops.load(std::memory_order_relaxed);
				if (DragFloat("Exposure", &stops, 0.05F, -16.0F, 16.0F, "%+.2f EV", ImGuiSliderFlags_AlwaysClamp))
					App().GetScene().GetSettings().exposureStops.store(stops, std::memory_order_relaxed);
				SetItemTooltip("Scales the image by 2^EV before tonemapping (with auto exposure, on top of what it picks). Drag, or double-click to type.");
				bool autoExposure = App().GetScene().GetSettings().autoExposure.load(std::memory_order_relaxed);
				if (MenuItem("Auto Exposure", nullptr, &autoExposure))
					App().GetScene().GetSettings().autoExposure.store(autoExposure, std::memory_order_relaxed);
				SetItemTooltip("Adapts the exposure to the scene's brightness over time.");
				if (autoExposure)
					TextDisabled("Auto: %+.2f EV", App().GetScene().GetAutoExposureStops());
				bool shadows = App().GetScene().GetSettings().shadows.load(std::memory_order_relaxed);
				if (MenuItem("Shadows", nullptr, &shadows))
					App().GetScene().GetSettings().shadows.store(shadows, std::memory_order_relaxed);
				SetItemTooltip("The lights' shadows: cascades for directional lights, cubes for point lights, a view per spot light.");
				bool culling = App().GetScene().GetSettings().culling.load(std::memory_order_relaxed);
				if (MenuItem("Culling", nullptr, &culling))
					App().GetScene().GetSettings().culling.store(culling, std::memory_order_relaxed);
				SetItemTooltip("Skips the chunks of the model outside each view's frustum, and each shadow view's.");
				bool clusteredLights = App().GetScene().GetSettings().clusteredLights.load(std::memory_order_relaxed);
				if (MenuItem("Clustered Lights", nullptr, &clusteredLights))
					App().GetScene().GetSettings().clusteredLights.store(clusteredLights, std::memory_order_relaxed);
				SetItemTooltip("Shades each pixel with only the lights that may reach its cluster (a tile of the view and a slice of depth), not all of them.");
				if (ui::BeginMenu("Tonemapper"))
				{
					static constexpr std::array<std::pair<const char*, uint32_t>, 5> kTonemappers{{
						{"Khronos PBR Neutral", TONEMAPPER_PBR_NEUTRAL},
						{"ACES", TONEMAPPER_ACES},
						{"AgX", TONEMAPPER_AGX},
						{"Reinhard", TONEMAPPER_REINHARD},
						{"Linear (clamped)", TONEMAPPER_LINEAR},
					}};
					auto current = App().GetScene().GetSettings().tonemapper.load(std::memory_order_relaxed);
					for (auto [name, tonemapper] : kTonemappers)
						if (MenuItem(name, nullptr, current == tonemapper))
							App().GetScene().GetSettings().tonemapper.store(tonemapper, std::memory_order_relaxed);
					ImGui::EndMenu();
				}
			}
			if (MenuItem("Lights...", nullptr, gShowLights))
				gShowLights = !gShowLights;
			SetItemTooltip("Lists the lights, and draws them over the views: where they are (a directional light outside the scene, shining in), spot cones, point light ranges and the shadow views' frusta.");
#if (SPEEDO_PROFILING_LEVEL > 0)
			if (MenuItem("Statistics...", nullptr, gShowStatistics))
				gShowStatistics = !gShowStatistics;
			SetItemTooltip("The passes' gpu timings (where the driver writes them), submits, transient memory, and in debug builds the vulkan objects.");
#endif
			ImGui::EndMenu();
		}
		if (ui::BeginMenu("About"))
		{
			if (MenuItem("Show IMGUI Demo..."))
				gShowDemoWindow = !gShowDemoWindow;
			Separator();
			if (MenuItem("About client..."))
				gShowAbout = !gShowAbout;
			ImGui::EndMenu();
		}

		EndMainMenuBar();
	}
	PopStyleVar(2);
	PopStyleColor();

	myImGui->EndFrame();
}

} // namespace client
