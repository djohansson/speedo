#include <core/file.h>
#include <core/task.h>
#include <gfx/environmentfilter.h>
#include <gfx/gpu.h>
#include <gfx/graphicsqueue.h>
#include <gfx/imguilayer.h>
#include <gfx/imgui_extra.h>
#include <gfx/meshimport.h>
#include <gfx/model.h>
#include <gfx/renderer.h>
#include <gfx/scene.h>
#include <gfx/shaderloader.h>
#include <gfx/shaders/capi.h>
#include <gfx/texture.h>
#include <client/windowedapplication.h>
#include <core/zip.h>
#include <platform/capi.h>
#include <platform/imguiplatform.h>
#include <rhi/capi.h>
#include <rhi/renderimageset.h>

#include <uuid.h>
#include <xxhash.h>

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <optional>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <limits>
#include <span>
#include <array>
#include <memory>
#include <utility>

//#include <imnodes.h>

namespace client
{

using namespace gfx;
using namespace rhi;
using QueueTimelineContext = gfx::QueueTimelineContext;
using CommandBufferHandle = gfx::CommandBufferHandle;
using Queue = gfx::Queue;
using QueueDeviceSyncInfo = gfx::QueueDeviceSyncInfo;
using Semaphore = gfx::Semaphore;
using SemaphoreCreateDesc = gfx::SemaphoreCreateDesc;
using SemaphoreHandle = gfx::SemaphoreHandle;
using RHI = gfx::RHI;

std::mutex WindowedApplication::gDrawMutex{};
core::LoadQueue WindowedApplication::gLoads{};
bool WindowedApplication::gShowAbout = false;
bool WindowedApplication::gShowDemoWindow = false;
bool WindowedApplication::gShowFps = false;
bool WindowedApplication::gShowTps = false;

namespace windowedapplication
{

[[nodiscard]] static WindowedApplication& App()
{
	auto app = std::static_pointer_cast<WindowedApplication>(core::Application::Get());
	ENSURE(app);
	return *app;
}

// gpu submits (and their batches) between the last two presented frames, from any thread. written by Draw
static std::atomic_uint32_t gFrameSubmitCount;
static std::atomic_uint32_t gFrameSubmitBatchCount;
// presented frames per second, over (at least) the last half second. written by Draw
static std::atomic<float> gFramesPerSecond;
static void LoadAndInstallModel(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress)
{
	App().GetScene().LoadModels({std::string(filePath)}, progress);
}

// extracts a zip archive into the user profile directory, once: later loads of the same file (by path, size and time)
// reuse it. returns the directory it was extracted to, or nothing if it failed (the reason is printed to stderr) or was
// cancelled.
static std::optional<std::filesystem::path> ExtractArchive(const std::filesystem::path& archive, std::atomic_uint8_t& progress)
{
	ZoneScopedN("WindowedApplication::ExtractArchive");

	auto app = core::Application::Get();
	auto userProfilePath = std::get<std::filesystem::path>(app->GetEnv().variables["UserProfilePath"]);

	std::error_code error;
	auto absolute = std::filesystem::absolute(archive, error);
	auto size = std::filesystem::file_size(archive, error);
	auto time = std::filesystem::last_write_time(archive, error).time_since_epoch().count();
	if (error)
	{
		std::println(stderr, "Failed to load archive {}: {}", archive.string(), error.message());
		return std::nullopt;
	}

	auto key = std::hash<std::string>{}(std::format("{}|{}|{}", absolute.string(), size, time));
	auto directory = userProfilePath / "archives" / std::format("{}-{:016x}", archive.stem().string(), key);
	auto marker = directory / ".extracted";
	if (std::filesystem::exists(marker, error))
		return directory;

	// a previous extraction that didn't finish
	std::filesystem::remove_all(directory, error);

	auto result = core::zip::ExtractAll(
		archive, directory, &app->GetExecutor(), &progress, [&app] { return app->IsExitRequested(); });
	if (!result)
	{
		if (!app->IsExitRequested())
			std::println(stderr, "Failed to load archive {}: {}", archive.string(), result.error());
		std::filesystem::remove_all(directory, error);
		return std::nullopt;
	}

	static constexpr std::array kMarker{std::byte{'\n'}};
	if (auto written = core::file::Write(marker, kMarker); !written)
		std::println(stderr, "Failed to write {}: {}", marker.string(), written.error().message());

	return directory;
}

// the model files (.obj, .gltf, .glb) below a directory, sorted
static std::vector<std::filesystem::path> FindModels(const std::filesystem::path& directory)
{
	std::vector<std::filesystem::path> models;
	std::error_code error;
	for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
		 !error && it != std::filesystem::recursive_directory_iterator();
		 it.increment(error))
	{
		if (mesh::IsModelFile(it->path()) && it->is_regular_file(error))
			models.push_back(it->path());
	}
	std::ranges::sort(models);
	return models;
}

// the models of an archive with several, for the user to choose from (see PrepareDraw)
struct ArchiveChoice
{
	std::string archive;
	std::filesystem::path directory;
	std::vector<std::filesystem::path> models;
};
static std::mutex gArchiveChoiceMutex;
static std::optional<ArchiveChoice> gArchiveChoice; // guarded by gArchiveChoiceMutex

// what LoadAndInstallArchive does with an archive of several models (sets of variants, such as the geodesic spheres)
enum class ArchiveModels : uint8_t
{
	kChoose, // the user chooses one, or all of them
	kAll, // all of them, side by side
};

// extracts a zip archive and loads the model in it. with several, see ArchiveModels. call from a load (see gLoads).
static void LoadAndInstallArchive(RHI& rhi, std::string_view archivePath, std::atomic_uint8_t& progress, ArchiveModels several)
{
	auto directory = ExtractArchive(archivePath, progress);
	if (!directory) // failed or cancelled
		return;

	auto models = FindModels(*directory);
	if (models.empty())
	{
		std::println(stderr, "Failed to load archive {}: it holds no model files", archivePath);
		return;
	}

	if (models.size() == 1 || several == ArchiveModels::kAll)
	{
		progress = 0;
		std::vector<std::string> paths;
		for (const auto& model : models)
			paths.push_back(model.string());
		App().GetScene().LoadModels(paths, progress);
		return;
	}

	std::scoped_lock lock(gArchiveChoiceMutex);
	gArchiveChoice = ArchiveChoice{
		.archive = std::filesystem::path(archivePath).filename().string(),
		.directory = std::move(*directory),
		.models = std::move(models)};
}

// loads the models below a directory (e.g. the encodings of a gltf sample model) as one, side by side (see Model::Load).
// call from a load (see gLoads).
static void LoadAndInstallFolder(RHI& rhi, std::string_view directoryPath, std::atomic_uint8_t& progress)
{
	auto models = FindModels(directoryPath);
	if (models.empty())
	{
		std::println(stderr, "Failed to load folder {}: it holds no model files", directoryPath);
		return;
	}

	std::vector<std::string> paths;
	for (const auto& model : models)
		paths.push_back(model.string());
	App().GetScene().LoadModels(paths, progress);
}

// the environment panorama files LoadAndInstallEnvironment takes, as a file dialog filter spec
static constexpr const char* kEnvironmentExtensions = "hdr";

// the image files LoadAndInstallImage takes, as a file dialog filter spec (see image::Import)
static constexpr const char* kImageExtensions = "jpg,jpeg,png,bmp,tga,gif,psd,pic,pnm,webp,ktx2";

[[nodiscard]] static bool IsImageFile(const std::filesystem::path& path)
{
	auto extension = path.extension().string();
	if (extension.empty())
		return false;
	std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::string_view extensions = kImageExtensions;
	for (size_t begin = 0; begin < extensions.size();)
	{
		auto end = std::min(extensions.find(',', begin), extensions.size());
		if (extensions.substr(begin, end - begin) == std::string_view(extension).substr(1))
			return true;
		begin = end + 1;
	}
	return false;
}

// loads whatever path is, by its type: a directory's models or a zip archive's (see ArchiveModels), a model, an
// environment panorama (.hdr), or an image (on the default material). call from a load (see gLoads).
static void LoadAndInstallFile(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress, ArchiveModels several)
{
	std::filesystem::path path(filePath);
	auto extension = path.extension().string();
	std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (std::error_code error; std::filesystem::is_directory(path, error))
		LoadAndInstallFolder(rhi, filePath, progress);
	else if (extension == ".zip")
		LoadAndInstallArchive(rhi, filePath, progress, several);
	else if (mesh::IsModelFile(path))
		LoadAndInstallModel(rhi, filePath, progress);
	else if (extension == ".hdr")
		App().GetScene().LoadEnvironment(std::string(filePath), progress);
	else if (IsImageFile(path))
		App().GetScene().LoadImage(filePath, progress);
	else
		std::println(stderr, "Failed to load file {}: not a model, zip archive, environment or image", filePath);
}

// recreates the swapchain at the current surface size, and everything sized after it. caller holds gDrawMutex.
// returns false if the surface has no area (minimized), in which case nothing is recreated.
bool RecreateWindowDependentObjects(RHI& rhi, platform::Window& window)
{
	ZoneScopedN("RecreateWindowDependentObjects");

	auto& device = rhi.GetPrimaryDevice();
	auto& swapchain = rhi.GetSwapchain(window);

	auto extent = swapchain.QuerySurfaceExtent();
	if (extent.width == 0 || extent.height == 0)
		return false;

	device.WaitIdle();
	swapchain.CreateSwapchain();

	// the window's size in screen coordinates, as its framebuffer's (the swapchain's) over its content scale
	auto& state = window.GetState();
	state.width = static_cast<uint32_t>(static_cast<float>(swapchain.GetDesc().extent.width) / state.xscale);
	state.height = static_cast<uint32_t>(static_cast<float>(swapchain.GetDesc().extent.height) / state.yscale);

	App().GetViews().OnResizeFramebuffer({extent.width, extent.height});
	App().GetViews().UpdateBuffers();
	App().GetScene().Resize();

	return true;
}

} // namespace windowedapplication

void WindowedApplication::PrepareDraw()
{
	ZoneScopedN("WindowedApplication::PrepareDraw");

	using namespace windowedapplication;
	using namespace ImGui;

	auto& rhi = GetRHI();
	auto& device = rhi.GetPrimaryDevice();

	myImGui->BeginFrame();

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	static bool gShowStatistics = false;
	{
		if (gShowStatistics)
		{
			if (Begin("Statistics", &gShowStatistics))
			{
				for (const auto& [name, count] : GetObjectCounts<kGraphicsApi>())
					Text("%.*s: %u", static_cast<int>(name.size()), name.data(), count);

				// gpu submits: totals, and how many went to the gpu between the last two presented frames (see Draw)
				Separator();
				Text(
					"Queue Submits: %llu (%u/frame)",
					static_cast<unsigned long long>(Queue::GetSubmitCount()),
					gFrameSubmitCount.load(std::memory_order_relaxed));
				Text(
					"Submit Batches: %llu (%u/frame)",
					static_cast<unsigned long long>(Queue::GetSubmitBatchCount()),
					gFrameSubmitBatchCount.load(std::memory_order_relaxed));

				// the frame graph's passes, on the gpu (see FrameGraph::ReadTimings): only where the backend writes their timestamps
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

		if (chosen || all || Button("Cancel"))
		{
			gShownArchiveChoice.reset();
			CloseCurrentPopup();
		}
		EndPopup();
	}

	if (gShowDemoWindow)
		ShowDemoWindow(&gShowDemoWindow);

	if (gShowAbout && Begin("About client", &gShowAbout))
	{
		End();
	}

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
						 ImGuiWindowFlags_AlwaysAutoResize))
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

	auto resourcePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]);

	// the file dialogs open in the test asset sets, if they have been fetched (see scripts/fetch-test-assets.ps1)
	auto dialogPath = [&resourcePath]
	{
		std::error_code error;
		auto testAssets = resourcePath / "test-assets";
		return (std::filesystem::is_directory(testAssets, error) ? testAssets : resourcePath).string();
	};

	// automation: SPEEDO_AUTOLOAD_MODEL (a model, or a zip archive or directory, whose models are loaded side by side)
	// and SPEEDO_AUTOLOAD_IMAGE (an image, on the default material) name files to load at startup, absolute or relative to
	// the resource directory, through the same load + install path as the "File" menu (SPEEDO_AUTOLOAD_SCENE=<index>: a
	// gltf model's scene, as View > Scene loads it). with
	// SPEEDO_AUTOLOAD_EXIT=<frames>, the application exits that many frames after the loads have finished (see
	// scripts/assettest.ps1).
	static std::vector<core::Future<void>> gAutoLoads;
	static std::optional<uint32_t> gAutoLoadExitFrames;
	if (static bool gAutoLoadDone = false; !gAutoLoadDone)
	{
		gAutoLoadDone = true;

		// queued as separate loads, which run concurrently
		// a zip archive loads all of its models, side by side
		std::optional<size_t> autoLoadScene;
		if (const char* scene = std::getenv("SPEEDO_AUTOLOAD_SCENE"); scene != nullptr && *scene != '\0')
			autoLoadScene = std::strtoull(scene, nullptr, 10);
		if (const char* autoLoadModel = std::getenv("SPEEDO_AUTOLOAD_MODEL"); autoLoadModel != nullptr && *autoLoadModel != '\0')
			gAutoLoads.emplace_back(gLoads.Enqueue(
				autoLoadModel,
				[&rhi, path = (resourcePath / autoLoadModel).string(), autoLoadScene](std::atomic_uint8_t& progress)
				{
					if (autoLoadScene && mesh::IsModelFile(path))
						App().GetScene().LoadModels({path}, progress, autoLoadScene);
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
	}

	// the installs are queued to the draw thread when the loads finish, so they have happened a frame later
	if (gAutoLoadExitFrames && std::ranges::all_of(gAutoLoads, [](const auto& load) { return load.IsReady(); }))
		if ((*gAutoLoadExitFrames)-- == 0)
			core::Application::Get()->RequestExit();

	if (BeginMainMenuBar())
	{
		if (BeginMenu("File"))
		{
			if (MenuItem("Open File..."))
			{
				// models, zip archives (of models), environments and images: what is loaded depends on the file's type
				static const std::string kAllExtensions = std::format("obj,gltf,glb,zip,{},{}", kEnvironmentExtensions, kImageExtensions);
				static const std::vector<platform::FileFilter> kFilterList = {
					platform::FileFilter{.name = "Models, zip archives, environments and images", .spec = kAllExtensions.c_str()},
					platform::FileFilter{.name = "Models (Wavefront OBJ, glTF)", .spec = "obj,gltf,glb"},
					platform::FileFilter{.name = "Zip archives", .spec = "zip"},
					platform::FileFilter{.name = "Environments (Radiance HDR panoramas)", .spec = kEnvironmentExtensions},
					platform::FileFilter{.name = "Images", .spec = kImageExtensions},
				};
				InternalOpenFileDialogueAsync(dialogPath(), kFilterList,
					[&rhi](std::string_view filePath, std::atomic_uint8_t& progressOut)
					{ LoadAndInstallFile(rhi, filePath, progressOut, ArchiveModels::kChoose); });
			}
			if (MenuItem("Open Folder..."))
			{
				InternalOpenFolderDialogueAsync(dialogPath(),
					[&rhi](std::string_view directoryPath, std::atomic_uint8_t& progressOut)
					{ LoadAndInstallFolder(rhi, directoryPath, progressOut); });
			}
			// if (MenuItem("Open Scene..."))
			// {
			// 	static const std::vector<platform::FileFilter> filterList = {
			// 		FileFilter{.name = "Scene files", .spec = "gltf,glb"}
			// 	};

			// 	InternalOpenFileDialogueAsync((resourcePath / "scenes").string(), filterList, 
			// 		[&scene = Scene{}](std::string_view filePath, std::atomic_uint8_t& progress){ scene::LoadScene(scene, filePath, progress); });
			// }
			Separator();
			if (MenuItem("Exit", "CTRL+Q"))
				core::Application::Get()->RequestExit();

			ImGui::EndMenu();
		}
		if (BeginMenu("View"))
		{
			// if (MenuItem("Node Editor..."))
			// 	showNodeEditor = !showNodeEditor;
			if (BeginMenu("Layout"))
			{
				// the views' grid is owned by the draw thread (see Views::OnResizeGrid), so track the
				// requested one here. nothing else changes the grid, so the window's is current when this is initialized.
				static Extent2d gSplitScreenGrid{.width = myViews->GetGrid().x, .height = myViews->GetGrid().y};
				Extent2d& splitScreenGrid = gSplitScreenGrid;

				//static bool hasChanged = 
				bool selected1x1 = splitScreenGrid.width == 1 && splitScreenGrid.height == 1;
				bool selected1x2 = splitScreenGrid.width == 1 && splitScreenGrid.height == 2;
				bool selected2x1 = splitScreenGrid.width == 2 && splitScreenGrid.height == 1;
				bool selected2x2 = splitScreenGrid.width == 2 && splitScreenGrid.height == 2;
				bool anyChanged = false;

				if (MenuItem("1x1", "Ctrl+1", &selected1x1) && selected1x1)
				{
					splitScreenGrid.width = 1;
					splitScreenGrid.height = 1;
					anyChanged = true;
				}
				else if (MenuItem("1x2", "Ctrl+2", &selected1x2) && selected1x2)
				{
					splitScreenGrid.width = 1;
					splitScreenGrid.height = 2;
					anyChanged = true;
				}
				else if (MenuItem("2x1", "Ctrl+3", &selected2x1) && selected2x1)
				{
					splitScreenGrid.width = 2;
					splitScreenGrid.height = 1;
					anyChanged = true;
				}
				else if (MenuItem("2x2", "Ctrl+4", &selected2x2) && selected2x2)
				{
					splitScreenGrid.width = 2;
					splitScreenGrid.height = 2;
					anyChanged = true;
				}

				ImGui::EndMenu();

				if (anyChanged)
				{
					// also upload the new views: otherwise that only happens on the next input change
					auto [resizeTask, resizeFuture] = core::CreateTask(
						[this, grid = splitScreenGrid]
						{
							myViews->OnResizeGrid({grid.width, grid.height});
							myViews->UpdateBuffers();
						});
					rhi.drawCalls.enqueue(resizeTask);
				}
			}
			if (BeginMenu("Environment"))
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
				if (MenuItem("Open Environment..."))
				{
					static const std::vector<platform::FileFilter> kFilterList = {
						platform::FileFilter{.name = "Environments (Radiance HDR panoramas)", .spec = kEnvironmentExtensions}};
					InternalOpenFileDialogueAsync(dialogPath(), kFilterList,
						[&rhi](std::string_view filePath, std::atomic_uint8_t& progressOut)
						{ App().GetScene().LoadEnvironment(std::string(filePath), progressOut); });
				}
				ImGui::EndMenu();
			}
			if (BeginMenu("Scene"))
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
							[&rhi, path = scenes.filePath, sceneIt](std::atomic_uint8_t& progress)
							{ App().GetScene().LoadModels({path}, progress, sceneIt); });
					PopID();
				}
				ImGui::EndMenu();
			}
			if (BeginMenu("Animation"))
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
			if (BeginMenu("Camera"))
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
				if (BeginMenu("Tonemapper"))
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
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
			{
				if (MenuItem("Statistics..."))
					gShowStatistics = !gShowStatistics;
			}
#endif
			ImGui::EndMenu();
		}
		if (BeginMenu("About"))
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

	myImGui->EndFrame();
}

void WindowedApplication::OnInputStateChanged(const core::InputState& input)
{
	using namespace windowedapplication;
	
	ZoneScopedN("WindowedApplication::OnInputStateChanged");

	auto& imguiIO = ImGui::GetIO();

	if (imguiIO.WantSaveIniSettings)
	{
		size_t iniStringSize;
		const char* iniString = ImGui::SaveIniSettingsToMemory(&iniStringSize);
		myImGuiIniSettings.assign(iniString, iniStringSize);
		imguiIO.WantSaveIniSettings = false;
	}

	if (!imguiIO.WantCaptureMouse && !imguiIO.WantCaptureKeyboard)
	{
		myViews->OnInputStateChanged(input);

		auto [updateTask, updateFuture] = core::CreateTask([this] { myViews->UpdateBuffers(); });
		GetRHI().drawCalls.enqueue(updateTask);
	}
}

bool WindowedApplication::Draw()
{
	using namespace windowedapplication;

	FrameMark;
	ZoneScopedN("WindowedApplication::Draw");

	std::unique_lock lock(gDrawMutex);
	std::vector<core::TaskHandle> frameTasks;

	auto& rhi = GetRHI();
	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = GetWindow(GetCurrentWindow());
	auto& swapchain = rhi.GetSwapchain(window);
	auto& pipeline = rhi.GetPrimaryDevice().GetPipeline();
	auto& executor = GetExecutor();

	if (window.IsMinimized())
		return false;

	// e.g. a fullscreen switch can leave the swapchain at the previous size without a matching resize event
	if (swapchain.NeedsRecreate() && !RecreateWindowDependentObjects(rhi, window))
		return false;

	auto [acquireNextImageSemaphore, lastFrameIndex, newFrameIndex, flipSuccess] = swapchain.Flip();

	// one lock at a time: queue types may alias the same context (and mutex), see Device::GetQueue
	auto queueFamilyIndex = [&device](QueueType type) { return device.GetQueue(type).Read()->queueFamilyIndex; };
	bool dedicatedTransfer = queueFamilyIndex(kQueueTypeTransfer) != queueFamilyIndex(kQueueTypeCompute);
	bool dedicatedCompute = queueFamilyIndex(kQueueTypeCompute) != queueFamilyIndex(kQueueTypeGraphics);

	// the timeline callbacks of the transfer queues (e.g. what the loaders' uploads release), unless the transfer queue
	// type aliases the graphics queues, whose callbacks are run below
	if (&device.GetQueue(kQueueTypeTransfer) != &device.GetQueue(kQueueTypeGraphics))
	{
		auto transfer = device.GetQueue(kQueueTypeTransfer).Read();
		for (const auto& [queue, submits] : transfer->queues)
			frameTasks.emplace_back(
				core::CreateTask([&executor, &queue = queue, &semaphore = transfer->semaphore]
				{ queue.SubmitCallbacks(executor, semaphore.GetValue()); }).handle);
	}

	if (flipSuccess)
	{
		auto& lastFrame = swapchain.GetFrames()[lastFrameIndex];
		auto& newFrame = swapchain.GetFrames()[newFrameIndex];

		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [lastGraphicsQueue, lastGraphicsSubmits] = graphics->queues.FetchAdd();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();

		frameTasks.emplace_back(
			core::CreateTask([&executor, &queue = graphicsQueue, &semaphore = graphics->semaphore]
			{ queue.SubmitCallbacks(executor, semaphore.GetValue()); }).handle);

		for (auto& fence : graphicsSubmits.fences)
			fence.Wait();

		graphicsSubmits = {};
		graphicsQueue.SwapAndResetPool();
		
		core::TaskHandle drawCall;
		while (rhi.drawCalls.try_dequeue(drawCall))
		{
			ZoneScopedN("WindowedApplication::Draw::drawCall");
			GetExecutor().Call(drawCall, graphics.Get().get());
		}

		// the frame's exposure histogram, and instance and joint buffers, now that the frame's previous use of them is done
		// (see the fences above)
		myScene->Update(*graphics, newFrameIndex);

		auto cmd = graphicsQueue.GetPool().Commands();

		ZoneScopedN("WindowedApplication::Draw::submit");

		GPU_SCOPE_COLLECT(cmd, graphicsQueue);

		std::vector<core::TaskHandle> graphicsCallbacks;
		myScene->Record(
			cmd,
			Scene::FrameTarget{
				.frameIndex = static_cast<uint16_t>(newFrameIndex),
				.swapchain = &swapchain,
				.swapchainFrame = &newFrame,
				.graphicsQueue = &graphicsQueue,
				.graphicsTimeline = graphics->timeline,
				.prepareUi = [this, &graphicsCallbacks](CommandBufferHandle cmd) { myImGui->PrepareFrame(cmd, graphicsCallbacks); },
				.drawUi = [this](CommandBufferHandle cmd) { myImGui->Draw(cmd); },
				.runInBackground =
					[this](std::function<void()> job)
				{
					auto [task, future] = core::CreateTask(std::move(job));
					GetExecutor().Submit(std::span(&task, 1));
				}});

		cmd.End();

		auto presentInfo = swapchain.PreparePresent();

		SemaphoreHandle acquireNextImageSemaphoreHandle = acquireNextImageSemaphore;
		auto graphicsDoneSemaphore = Semaphore(
			SemaphoreCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("graphicsDoneSemaphore{}", newFrameIndex)),
				SemaphoreType::kBinary
			});
		SemaphoreHandle graphicsDoneSemaphoreHandle = graphicsDoneSemaphore;
		// keeps the semaphore alive until the gpu has waited on it. the acquire fence is owned (and waited on) by the
		// swapchain instead, see Swapchain::myAcquireFences
		graphicsCallbacks.emplace_back(
			core::CreateTask(
				[acquireNextImageSemaphore = std::move(acquireNextImageSemaphore)] {}).handle);
		graphicsCallbacks.emplace_back(
			core::CreateTask(
				[&swapchain, presentIds = std::move(presentInfo.presentIds),
				 graphicsDoneSemaphore = std::move(graphicsDoneSemaphore)]
				 {
					for (auto presentId : presentIds)
						swapchain.WaitPresent(presentId);
				 }).handle);

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
			.waitSemaphores = {graphics->semaphore, acquireNextImageSemaphoreHandle},
			// the acquired image may still be in use by the presentation engine until the acquire semaphore signals:
			// nothing may touch it before that (PipelineStage::kNone waited for nothing, which flickered in fullscreen)
			.waitDstStageMasks = {PipelineStage::kAllGraphics, PipelineStage::kAllCommands},
			.waitSemaphoreValues = {lastGraphicsSubmits.maxTimelineValue, 1},
			.signalSemaphores = {graphics->semaphore, graphicsDoneSemaphoreHandle},
			.signalSemaphoreValues = {++graphics->timeline, 1},
			.callbacks = std::move(graphicsCallbacks)});

		graphicsSubmits |= graphicsQueue.Submit();

		presentInfo.waitSemaphores.emplace_back(graphicsDoneSemaphoreHandle);

		if (dedicatedCompute)
		{
			auto compute = device.GetQueue(kQueueTypeCompute).Write();
			auto& [computeQueue, computeSubmits] = compute->queues.FetchAdd();

			for (auto& fence : computeSubmits.fences)
				fence.Wait();

			computeSubmits = {};
			computeQueue.SwapAndResetPool();

			computeQueue.EnqueuePresent(std::move(presentInfo));
			PresentResult presentResult = PresentResult::kSuccess;
			computeSubmits |= computeQueue.Present(&presentResult);
			swapchain.OnPresentResult(presentResult);
		}
		else
		{
			graphicsQueue.EnqueuePresent(std::move(presentInfo));
			PresentResult presentResult = PresentResult::kSuccess;
			graphicsSubmits |= graphicsQueue.Present(&presentResult);
			swapchain.OnPresentResult(presentResult);
		}

		static uint64_t gLastFrameSubmitCount = Queue::GetSubmitCount();
		static uint64_t gLastFrameSubmitBatchCount = Queue::GetSubmitBatchCount();
		auto submitCount = Queue::GetSubmitCount();
		auto submitBatchCount = Queue::GetSubmitBatchCount();
		gFrameSubmitCount.store(static_cast<uint32_t>(submitCount - gLastFrameSubmitCount), std::memory_order_relaxed);
		gFrameSubmitBatchCount.store(static_cast<uint32_t>(submitBatchCount - gLastFrameSubmitBatchCount), std::memory_order_relaxed);
		gLastFrameSubmitCount = submitCount;
		gLastFrameSubmitBatchCount = submitBatchCount;

		using namespace std::chrono_literals;
		static auto gFpsTime = std::chrono::steady_clock::now();
		static uint32_t gFpsFrameCount = 0;
		gFpsFrameCount++;
		if (auto now = std::chrono::steady_clock::now(); now - gFpsTime >= 500ms)
		{
			gFramesPerSecond.store(
				static_cast<float>(gFpsFrameCount) / std::chrono::duration<float>(now - gFpsTime).count(),
				std::memory_order_relaxed);
			gFpsFrameCount = 0;
			gFpsTime = now;
		}
	}

	GetExecutor().Submit(frameTasks);

	return flipSuccess;
}

WindowedApplication::WindowedApplication(
	std::string_view appName, core::Environment&& env, CreateWindowFunc createWindowFunc)
	: platform::WindowedApplication(appName, std::forward<core::Environment>(env), createWindowFunc)
	, myRHI(std::make_unique<RHI>(RHIInitializationData{
		  .name = appName,
		  .window = GetCurrentWindow(), // the window the platform made: the one we draw in
		  .descriptorPoolSizes = Scene::DescriptorPoolSizes()}))
{
	using namespace windowedapplication;

	auto& rhi = GetRHI();
	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = GetWindow(GetCurrentWindow());
	auto& swapchain = rhi.GetSwapchain(window);
	auto& pipeline = device.GetPipeline();

	myViews = std::make_unique<Views>(device, glm::uvec2(swapchain.GetDesc().extent.width, swapchain.GetDesc().extent.height));

	myScene = std::make_unique<Scene>(rhi, *myViews);

	{
		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
		myImGui = std::make_unique<ImGuiLayer>(window, swapchain, rhi, graphicsQueue, graphics->queues.Capacity(), myImGuiIniSettings);
	}
}

WindowedApplication::~WindowedApplication()
{
	// can't tear down the rhi here, since Application::Get() already returns null (see Shutdown())
	ENSUREF(!myRHI, "WindowedApplication::Shutdown() must be called before the application is released");
}

void WindowedApplication::Shutdown()
{
	using namespace windowedapplication;

	ZoneScopedN("WindowedApplication::Shutdown");

	if (!myRHI)
		return;

	auto& rhi = GetRHI();
	auto& device = rhi.GetPrimaryDevice();
	auto& executor = GetExecutor();

	// let in-flight frame tasks (from the last Draw) and loads finish before tearing anything down
	executor.JoinAll();

	// settle: run the draw calls the draw task didn't get to, wait for the gpu, and run the timeline callbacks that are
	// now due, until no draw calls remain. an unrun task is never destroyed, and would leak what it holds past the device:
	// e.g. a load completing during shutdown queues an install, and installing an image queues another draw call from
	// its transition's timeline callback.
	for (bool settled = false; !settled;)
	{
		settled = true;
		{
			auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
			core::TaskHandle drawCall;
			while (rhi.drawCalls.try_dequeue(drawCall))
			{
				executor.Call(drawCall, graphics.Get().get());
				settled = false;
			}
		}
		executor.JoinAll();

		device.WaitIdle();

		// all gpu work has completed, so every pending timeline callback is due (they own per-frame fences/semaphores etc).
		// queue locks are released before joining, since callbacks may access the queues themselves.
		// each distinct queue context once, locking one at a time: queue types may alias the same context (and mutex).
		{
			constexpr auto kAllTimelineValues = std::numeric_limits<uint64_t>::max();

			std::vector<QueueTimelineContext*> visited;
			for (auto type : {kQueueTypeGraphics, kQueueTypeCompute, kQueueTypeTransfer})
			{
				auto* context = &device.GetQueue(type);
				if (std::ranges::contains(visited, context))
					continue;
				visited.emplace_back(context);

				auto queues = context->Write();
				for (auto& [queue, submits] : queues->queues)
					queue.SubmitCallbacks(executor, kAllTimelineValues);
			}
		}
		executor.JoinAll();

		if (rhi.drawCalls.size_approx() != 0)
			settled = false;
	}

	myImGui.reset();

	// what holds gpu objects, before the rhi they belong to
	myScene.reset();
	myViews.reset();
	myRHI.reset();
}

void WindowedApplication::OnResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace windowedapplication;

	std::unique_lock lock(gDrawMutex);

	ZoneScopedN("WindowedApplication::OnResizeFramebuffer");

	// minimizing reports 0x0: keep the swapchain, and have Draw skip frames until the window is restored
	platform::WindowedApplication::OnResizeFramebuffer(window, width, height);
	auto& platformWindow = GetWindow(window);
	if (platformWindow.IsMinimized())
		return;

	// the swapchain is sized after the surface (which already has this size) rather than width/height
	RecreateWindowDependentObjects(GetRHI(), platformWindow);
}

} // namespace client
