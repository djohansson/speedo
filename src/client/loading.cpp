#include "windowedapplicationdetail.h"

#include <core/application.h>
#include <core/utils.h>
#include <core/zip.h>

#include <algorithm>
#include <ranges>

namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE::windowedapplication
{

void LoadAndInstallModel(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress, ModelGround ground, LoadChoice choice)
{
	if (choice == LoadChoice::kAsk)
		if (auto variants = mesh::VariantNames(filePath); !variants.empty())
		{
			std::scoped_lock lock(gVariantChoiceMutex);
			gVariantChoice = VariantChoice{.filePath = std::string(filePath), .variants = std::move(variants), .ground = ground};
			return;
		}

	App().GetScene().LoadModels({std::string(filePath)}, progress, {}, ModelArrangement::kSideBySide, ground);
}

// extracts a zip archive into the user profile directory, once: later loads of the same file (by path, size and time)
// reuse it. returns the directory it was extracted to, or nothing if it failed (the reason is printed to stderr) or was
// cancelled.
std::optional<std::filesystem::path> ExtractArchive(const std::filesystem::path& archive, std::atomic_uint8_t& progress)
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
	// done, by this extractor: its marker's length says which (1 byte, before an entry shorter than a page was cut to its
	// size: its file kept the tail of the page mio made it, which failed geodesic.zip's small objs), and an older one's
	// extraction is done again, in the same place
	static constexpr std::array kMarker{std::byte{'2'}, std::byte{'\n'}};
	auto marker = directory / ".extracted";
	if (std::filesystem::exists(marker, error) && std::filesystem::file_size(marker, error) == kMarker.size() && !error)
		return directory;
	error.clear();

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

	if (auto written = core::file::Write(marker, kMarker); !written)
		std::println(stderr, "Failed to write {}: {}", marker.string(), written.error().message());

	return directory;
}

// the model files (.obj, .gltf, .glb) below a directory, sorted
std::vector<std::filesystem::path> FindModels(const std::filesystem::path& directory)
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

// extracts a zip archive and loads the model in it. with several, see LoadChoice. call from a load (see gLoads).
void LoadAndInstallArchive(RHI& rhi, std::string_view archivePath, std::atomic_uint8_t& progress, LoadChoice choice, ModelGround ground)
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

	if (models.size() == 1)
	{
		progress = 0;
		LoadAndInstallModel(rhi, models.front().string(), progress, ground, choice);
		return;
	}

	if (choice == LoadChoice::kDefault)
	{
		progress = 0;
		std::vector<std::string> paths;
		for (const auto& model : models)
			paths.push_back(model.string());
		App().GetScene().LoadModels(paths, progress, {}, ModelArrangement::kSideBySide, ground);
		return;
	}

	std::scoped_lock lock(gArchiveChoiceMutex);
	gArchiveChoice = ArchiveChoice{
		.archive = std::filesystem::path(archivePath).filename().string(),
		.directory = std::move(*directory),
		.models = std::move(models),
		.ground = ground};
}

// loads the models below a directory (e.g. the encodings of a gltf sample model) as one, side by side (see Model::Load).
// call from a load (see gLoads).
void LoadAndInstallFolder(RHI& rhi, std::string_view directoryPath, std::atomic_uint8_t& progress, ModelGround ground)
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
	App().GetScene().LoadModels(paths, progress, {}, ModelArrangement::kSideBySide, ground);
}


[[nodiscard]] bool IsImageFile(const std::filesystem::path& path)
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

// loads whatever path is, by its type: a directory's models or a zip archive's, a model (choice: what of either it asks
// the user, see LoadChoice), an environment panorama (.hdr), or an image (on the default material). call from a load
// (see gLoads).
void LoadAndInstallFile(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress, LoadChoice choice, ModelGround ground)
{
	std::filesystem::path path(filePath);
	auto extension = path.extension().string();
	std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (std::error_code error; std::filesystem::is_directory(path, error))
		LoadAndInstallFolder(rhi, filePath, progress, ground);
	else if (extension == ".zip")
		LoadAndInstallArchive(rhi, filePath, progress, choice, ground);
	else if (mesh::IsModelFile(path))
		LoadAndInstallModel(rhi, filePath, progress, ground, choice);
	else if (extension == ".hdr")
		App().GetScene().LoadEnvironment(std::string(filePath), progress);
	else if (IsImageFile(path))
		App().GetScene().LoadImage(filePath, progress);
	else
		std::println(stderr, "Failed to load file {}: not a model, zip archive, environment or image", filePath);
}

// the recent files, newest first, read from the user profile once (see RecentFiles). ui thread
static std::vector<RecentFile> gRecentFiles;
static bool gRecentFilesRead = false;

[[nodiscard]] static std::filesystem::path RecentFilesPath()
{
	return std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / "recent.txt";
}

// a line a recent file: its options (side-by-side or as-authored, then +ground if it stands on one), then its paths,
// each after a tab
static void WriteRecentFiles()
{
	std::string text;
	for (const auto& recent : gRecentFiles)
	{
		text += recent.arrangement == ModelArrangement::kAsAuthored ? "as-authored" : "side-by-side";
		text += recent.ground == ModelGround::kPlane ? "+ground" : "";
		for (const auto& path : recent.paths)
			text += std::format("\t{}", path);
		text += '\n';
	}
	auto path = RecentFilesPath();
	if (auto written = core::file::Write(path, std::as_bytes(std::span(text))); !written)
		std::println(stderr, "Failed to write {}: {}", path.string(), written.error().message());
}

const std::vector<RecentFile>& RecentFiles()
{
	if (gRecentFilesRead)
		return gRecentFiles;
	gRecentFilesRead = true;

	auto mapped = core::file::Map(RecentFilesPath());
	if (!mapped || mapped->size() == 0) // none yet
		return gRecentFiles;
	std::string_view text(reinterpret_cast<const char*>(mapped->data()), mapped->size());
	for (auto line : text | std::views::split('\n'))
	{
		std::string_view row(line.begin(), line.end());
		if (row.ends_with('\r'))
			row.remove_suffix(1);
		RecentFile recent;
		bool options = true;
		for (auto field : row | std::views::split('\t'))
		{
			std::string_view value(field.begin(), field.end());
			if (std::exchange(options, false))
			{
				recent.arrangement = value.starts_with("as-authored") ? ModelArrangement::kAsAuthored : ModelArrangement::kSideBySide;
				recent.ground = value.ends_with("+ground") ? ModelGround::kPlane : ModelGround::kNone;
			}
			else if (!value.empty())
				recent.paths.emplace_back(value);
		}
		if (!recent.paths.empty() && gRecentFiles.size() < kRecentFileCount)
			gRecentFiles.push_back(std::move(recent));
	}
	return gRecentFiles;
}

void AddRecentFile(RecentFile file)
{
	(void)RecentFiles();
	std::erase(gRecentFiles, file);
	gRecentFiles.insert(gRecentFiles.begin(), std::move(file));
	if (gRecentFiles.size() > kRecentFileCount)
		gRecentFiles.resize(kRecentFileCount);
	WriteRecentFiles();
}

// the units remembered for files (see RememberedUnit), read from the user profile once. ui thread
static core::UnorderedMap<std::string, float> gRememberedUnits;
static bool gRememberedUnitsRead = false;

[[nodiscard]] static std::filesystem::path RememberedUnitsPath()
{
	return std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / "units.txt";
}

static void ReadRememberedUnits()
{
	if (std::exchange(gRememberedUnitsRead, true))
		return;
	auto mapped = core::file::Map(RememberedUnitsPath());
	if (!mapped || mapped->size() == 0) // none yet
		return;
	// a line a file: its meters per unit, then its path after a tab
	std::string_view text(reinterpret_cast<const char*>(mapped->data()), mapped->size());
	for (auto line : text | std::views::split('\n'))
	{
		std::string_view row(line.begin(), line.end());
		if (row.ends_with('\r'))
			row.remove_suffix(1);
		auto tab = row.find('\t');
		if (tab == std::string_view::npos || tab + 1 >= row.size())
			continue;
		float metersPerUnit = std::strtof(std::string(row.substr(0, tab)).c_str(), nullptr);
		if (metersPerUnit > 0.0F)
			gRememberedUnits[std::string(row.substr(tab + 1))] = metersPerUnit;
	}
}

static void WriteRememberedUnits()
{
	std::vector<std::string_view> paths;
	paths.reserve(gRememberedUnits.size());
	for (const auto& [path, metersPerUnit] : gRememberedUnits)
		paths.emplace_back(path);
	std::ranges::sort(paths);
	std::string text;
	for (auto path : paths)
		text += std::format("{:.6g}\t{}\n", static_cast<double>(gRememberedUnits[std::string(path)]), path);
	auto path = RememberedUnitsPath();
	if (auto written = core::file::Write(path, std::as_bytes(std::span(text))); !written)
		std::println(stderr, "Failed to write {}: {}", path.string(), written.error().message());
}

std::optional<float> RememberedUnit(std::string_view filePath)
{
	ReadRememberedUnits();
	auto found = gRememberedUnits.find(std::string(filePath));
	return found != gRememberedUnits.end() ? std::optional(found->second) : std::nullopt;
}

void RememberUnit(std::string_view filePath, std::optional<float> metersPerUnit)
{
	ReadRememberedUnits();
	if (metersPerUnit)
		gRememberedUnits[std::string(filePath)] = *metersPerUnit;
	else if (gRememberedUnits.erase(std::string(filePath)) == 0)
		return;
	WriteRememberedUnits();
}

void ClearRecentFiles()
{
	gRecentFiles.clear();
	gRecentFilesRead = true;
	WriteRecentFiles();
}

std::string RecentFileName(const RecentFile& file)
{
	if (file.paths.empty())
		return {};
	auto name = std::filesystem::path(file.paths.front()).filename().string();
	return file.paths.size() == 1 ? name : std::format("{} and {} more", name, file.paths.size() - 1);
}

std::string RecentFileDetail(const RecentFile& file)
{
	if (file.paths.empty())
		return {};
	// the folder holding them all (a gltf sample model's encodings are each in a folder named after the encoding)
	auto folder = std::filesystem::path(file.paths.front()).parent_path();
	for (const auto& path : file.paths)
		while (!folder.empty() && std::filesystem::path(path).lexically_relative(folder).string().starts_with(".."))
			folder = folder.parent_path();
	auto detail = folder.filename().string();
	if (file.paths.size() > 1)
		detail += file.arrangement == ModelArrangement::kAsAuthored ? ", as authored" : ", side by side";
	if (file.ground == ModelGround::kPlane)
		detail += ", ground plane";
	return detail;
}

} // namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE::windowedapplication
