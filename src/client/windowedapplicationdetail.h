#pragma once

// what the client's windowed application's sources (windowedapplication.cpp: the frame loop, ui.cpp: the user interface,
// loading.cpp: the load policy) share. internal to them.

#include <core/file.h>
#include <core/task.h>
#include <gfx/environmentfilter.h>
#include <gfx/gpu.h>
#include <gfx/graphicsqueue.h>
#include <gfx/imguirenderer.h>
#include <gfx/meshimport.h>
#include <gfx/model.h>
#include <gfx/renderer.h>
#include <gfx/scene.h>
#include <gfx/shaderloader.h>
#include <gfx/shaders/capi.h>
#include <gfx/texture.h>
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
#include <thread>
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
#include <client/windowedapplication.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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

namespace windowedapplication
{

[[nodiscard]] inline WindowedApplication& App()
{
	auto app = std::static_pointer_cast<WindowedApplication>(core::Application::Get());
	ENSURE(app);
	return *app;
}

// presented frames per second, and gpu submits (vkQueueSubmit calls) and their batches (VkSubmitInfos) per presented frame,
// over (at least) the last half second, from any thread. written by Draw
// SPEEDO_CAPTURE=<file>: the frame an autoload exit ends on, read back and saved as a bmp (32 bits, top down), for
// comparing renders without the screen (a locked one can't be captured). the ui asks for it (1), the draw thread records
// its read back (2), and its graphics timeline callback saves it (3), after which the ui exits
inline std::atomic_uint8_t gCaptureState{0};
inline std::string gCapturePath;
inline std::atomic<float> gFramesPerSecond;
// the frames Draw has submitted (SPEEDO_AUTOLOAD_EXIT counts them: the ui thread's frames aren't paced by them)
inline std::atomic_uint64_t gDrawnFrameCount{0};
inline std::atomic<float> gSubmitsPerFrame;
inline std::atomic<float> gSubmitBatchesPerFrame;

// the models of an archive with several, for the user to choose from (see PrepareDraw)
struct ArchiveChoice
{
	std::string archive;
	std::filesystem::path directory;
	std::vector<std::filesystem::path> models;
	ModelGround ground = ModelGround::kNone; // what they stand on (see ModelGround)
};
inline std::mutex gArchiveChoiceMutex;
inline std::optional<ArchiveChoice> gArchiveChoice; // guarded by gArchiveChoiceMutex

// the material variants of a model file opened, for the user to choose which to lay out (see PrepareDraw)
struct VariantChoice
{
	std::string filePath;
	std::vector<std::string> variants;
	ModelGround ground = ModelGround::kNone;
};
inline std::mutex gVariantChoiceMutex;
inline std::optional<VariantChoice> gVariantChoice; // guarded by gVariantChoiceMutex

// whether a load asks the user what it holds several of (see LoadAndInstallFile): which of an archive's models to load
// (sets of variants, such as the geodesic spheres), and which of a model's material variants to lay out (see
// Scene::LoadVariants), as File > Open and Open Recent do; or loads the default, an archive's models all side by side and
// a model with its default materials, as the autoloads do (what they load, their variables say)
enum class LoadChoice : uint8_t
{
	kAsk,
	kDefault,
};

// escape closes the window being built (called between its Begin and End) while it, or a child of it, has the focus:
// Shortcut's default route is the focused window's. a menu or popup open over it takes escape first, as does an active
// text field. ui thread
inline void CloseOnEscape(bool& open)
{
	if (ImGui::Shortcut(ImGuiKey_Escape))
		open = false;
}

// whether the file browser (File > Open..., see WindowedApplication::InternalDrawBrowser) is open. ui thread
inline bool gShowBrowser = false;
// what it is open for: files of any kind (File > Open...), or an environment (View > Environment > Open
// Environment..., panoramas only). each keeps its own listing options. ui thread
enum class BrowserPurpose : uint8_t
{
	kFiles,
	kEnvironment,
};
inline BrowserPurpose gBrowserPurpose = BrowserPurpose::kFiles;

// the environment panorama files gfx::Scene::LoadEnvironment takes, as a file dialog filter spec
inline constexpr const char* kEnvironmentExtensions = "hdr";

// the image files gfx::Scene::LoadImage takes, as a file dialog filter spec (see image::Import)
inline constexpr const char* kImageExtensions = "jpg,jpeg,png,bmp,tga,gif,psd,pic,pnm,webp,ktx2";

// loads a model standing on ground (see ModelGround), or with choice kAsk where it has material variants, offers them to
// lay out (see VariantChoice). call from a load (see gLoads).
void LoadAndInstallModel(
	RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress, ModelGround ground = ModelGround::kNone, LoadChoice choice = LoadChoice::kDefault);

// extracts a zip archive into the user profile directory, once: later loads of the same file (by path, size and time)
// reuse it. returns the directory it was extracted to, or nothing if it failed (the reason is printed to stderr) or was
// cancelled.
std::optional<std::filesystem::path> ExtractArchive(const std::filesystem::path& archive, std::atomic_uint8_t& progress);

// the model files (.obj, .gltf, .glb) below a directory, sorted
std::vector<std::filesystem::path> FindModels(const std::filesystem::path& directory);

// extracts a zip archive and loads the model in it. with several, see LoadChoice. call from a load (see gLoads).
void LoadAndInstallArchive(
	RHI& rhi, std::string_view archivePath, std::atomic_uint8_t& progress, LoadChoice choice, ModelGround ground = ModelGround::kNone);

// loads the models below a directory (e.g. the encodings of a gltf sample model) as one, side by side (see Model::Load).
// call from a load (see gLoads).
void LoadAndInstallFolder(RHI& rhi, std::string_view directoryPath, std::atomic_uint8_t& progress, ModelGround ground = ModelGround::kNone);

[[nodiscard]] bool IsImageFile(const std::filesystem::path& path);

// loads whatever path is, by its type: a directory's models or a zip archive's, a model (choice: what of either it asks
// the user, see LoadChoice), an environment panorama (.hdr), or an image (on the default material). the models stand on
// ground (see ModelGround). call from a load (see gLoads).
void LoadAndInstallFile(
	RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress, LoadChoice choice, ModelGround ground = ModelGround::kNone);

// what was opened (File > Open... or Open Recent): a file of any kind the client loads (see LoadAndInstallFile), or
// several models as one (see Scene::LoadModels), arranged and standing on a ground as the browser said
struct RecentFile
{
	std::vector<std::string> paths;
	ModelArrangement arrangement = ModelArrangement::kSideBySide; // of several models
	ModelGround ground = ModelGround::kNone;

	bool operator==(const RecentFile&) const = default;
};
// the files opened last (File > Open Recent), newest first, at most kRecentFileCount of them: read from the user profile
// (recent.txt) when first asked for, and written back whenever they change. ui thread
inline constexpr size_t kRecentFileCount = 6;
[[nodiscard]] const std::vector<RecentFile>& RecentFiles();
// makes it the newest (moved there if it is in the list already, the oldest dropped past kRecentFileCount)
void AddRecentFile(RecentFile file);
void ClearRecentFiles();
// what the menu calls it: the file's name, or the first's and how many more; and beside it, the folder holding them and
// how they were opened (several side by side or as authored, on a ground plane)
[[nodiscard]] std::string RecentFileName(const RecentFile& file);
[[nodiscard]] std::string RecentFileDetail(const RecentFile& file);

// recreates the swapchain at the current surface size, and everything sized after it. caller holds gDrawMutex.
// returns false if the surface has no area (minimized), in which case nothing is recreated.
bool RecreateWindowDependentObjects(RHI& rhi, platform::Window& window);

} // namespace windowedapplication

} // namespace client
