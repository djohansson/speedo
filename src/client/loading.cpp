#include "windowedapplicationdetail.h"

#include <core/application.h>
#include <core/zip.h>

namespace client::windowedapplication
{

void LoadAndInstallModel(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress)
{
	App().GetScene().LoadModels({std::string(filePath)}, progress);
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

// extracts a zip archive and loads the model in it. with several, see ArchiveModels. call from a load (see gLoads).
void LoadAndInstallArchive(RHI& rhi, std::string_view archivePath, std::atomic_uint8_t& progress, ArchiveModels several)
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
void LoadAndInstallFolder(RHI& rhi, std::string_view directoryPath, std::atomic_uint8_t& progress)
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

// loads whatever path is, by its type: a directory's models or a zip archive's (see ArchiveModels), a model, an
// environment panorama (.hdr), or an image (on the default material). call from a load (see gLoads).
void LoadAndInstallFile(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress, ArchiveModels several)
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

} // namespace client::windowedapplication
