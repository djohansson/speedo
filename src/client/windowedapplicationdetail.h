#pragma once

// what the client's windowed application's sources (windowedapplication.cpp: the frame loop, ui.cpp: the user interface,
// loading.cpp: the load policy) share. internal to them.

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

// gpu submits (and their batches) between the last two presented frames, from any thread. written by Draw
inline std::atomic_uint32_t gFrameSubmitCount;
inline std::atomic_uint32_t gFrameSubmitBatchCount;
// presented frames per second, over (at least) the last half second. written by Draw
inline std::atomic<float> gFramesPerSecond;

// the models of an archive with several, for the user to choose from (see PrepareDraw)
struct ArchiveChoice
{
	std::string archive;
	std::filesystem::path directory;
	std::vector<std::filesystem::path> models;
};
inline std::mutex gArchiveChoiceMutex;
inline std::optional<ArchiveChoice> gArchiveChoice; // guarded by gArchiveChoiceMutex

// what LoadAndInstallArchive does with an archive of several models (sets of variants, such as the geodesic spheres)
enum class ArchiveModels : uint8_t
{
	kChoose, // the user chooses one, or all of them
	kAll, // all of them, side by side
};

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

void LoadAndInstallModel(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress);

// extracts a zip archive into the user profile directory, once: later loads of the same file (by path, size and time)
// reuse it. returns the directory it was extracted to, or nothing if it failed (the reason is printed to stderr) or was
// cancelled.
std::optional<std::filesystem::path> ExtractArchive(const std::filesystem::path& archive, std::atomic_uint8_t& progress);

// the model files (.obj, .gltf, .glb) below a directory, sorted
std::vector<std::filesystem::path> FindModels(const std::filesystem::path& directory);

// extracts a zip archive and loads the model in it. with several, see ArchiveModels. call from a load (see gLoads).
void LoadAndInstallArchive(RHI& rhi, std::string_view archivePath, std::atomic_uint8_t& progress, ArchiveModels several);

// loads the models below a directory (e.g. the encodings of a gltf sample model) as one, side by side (see Model::Load).
// call from a load (see gLoads).
void LoadAndInstallFolder(RHI& rhi, std::string_view directoryPath, std::atomic_uint8_t& progress);

[[nodiscard]] bool IsImageFile(const std::filesystem::path& path);

// loads whatever path is, by its type: a directory's models or a zip archive's (see ArchiveModels), a model, an
// environment panorama (.hdr), or an image (on the default material). call from a load (see gLoads).
void LoadAndInstallFile(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress, ArchiveModels several);

// recreates the swapchain at the current surface size, and everything sized after it. caller holds gDrawMutex.
// returns false if the surface has no area (minimized), in which case nothing is recreated.
bool RecreateWindowDependentObjects(RHI& rhi, platform::Window& window);

} // namespace windowedapplication

} // namespace client
