#include "file.h"

#include <ctime>
#include <chrono>

namespace file
{

namespace detail
{

const char* ToString(AssetManifestErrorCode code) noexcept
{
	switch (code)
	{
	case AssetManifestErrorCode::kMissing: return "Missing";
	case AssetManifestErrorCode::kInvalidLocation: return "InvalidLocation";
	case AssetManifestErrorCode::kInvalidSourceFile: return "InvalidSourceFile";
	case AssetManifestErrorCode::kInvalidCacheFile: return "InvalidCacheFile";
	}

	return "Unknown";
}

} // namespace detail

std::expected<std::string, std::error_code>
GetTimeStamp(const std::filesystem::path& filePath) noexcept
{
	ZoneScoped;

	std::error_code error;

	std::time_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::filesystem::last_write_time(filePath, error).time_since_epoch())).count();

	if (error)
		return std::unexpected(error);
	
	static constexpr size_t kBufferSize = 80;
	std::array<char, kBufferSize> buffer;

	auto time = std::localtime(&timestamp);

	if (!time)
		return std::unexpected(std::make_error_code(std::errc::invalid_argument));

	auto timeStrSize = std::strftime(buffer.data(), buffer.size(), "%c", time);

	if (timeStrSize == 0)
		return std::unexpected(std::make_error_code(std::errc::invalid_argument));

	return std::string(buffer.data(), timeStrSize);
}

std::expected<std::filesystem::path, std::error_code>
GetCanonicalPath(const char* pathStr, const char* defaultPathStr, bool createIfMissing) noexcept
{
	ASSERT(defaultPathStr != nullptr);

	std::error_code error;

	auto path = std::filesystem::path((pathStr != nullptr) ? pathStr : defaultPathStr);

	if (createIfMissing && !std::filesystem::exists(path, error) && !error)
		std::filesystem::create_directories(path, error);
	
	if (error)
		return std::unexpected(error);

	//ASSERT(std::filesystem::is_directory(path, error));

	path = std::filesystem::canonical(path, error);

	if (error)
		return std::unexpected(error);

	return path;
}

std::expected<Record, std::error_code> LoadAsset(
	const std::filesystem::path& assetFilePath,
	const LoadFn& loadSourceFileFn,
	const LoadFn& loadBinaryCacheFn,
	const SaveFn& saveBinaryCacheFn,
	const std::string& parameterHash)
{
	using namespace detail;
	
	ZoneScoped;

	auto rootPath = std::get<std::filesystem::path>(Application::Instance().lock()->Env().variables["RootPath"]);
	auto cacheDir = std::get<std::filesystem::path>(Application::Instance().lock()->Env().variables["UserProfilePath"]);
	auto cacheDirStatus = std::filesystem::status(cacheDir);
	if (!std::filesystem::exists(cacheDirStatus) ||
		!std::filesystem::is_directory(cacheDirStatus))
		std::filesystem::create_directories(cacheDir);

	std::error_code error;
	std::filesystem::path manifestPath(cacheDir / std::filesystem::relative(assetFilePath, rootPath, error));
	if (error)
		return std::unexpected(error);

	manifestPath /= parameterHash + ".json";

	auto manifestStatus = std::filesystem::status(manifestPath);

	auto importSourceFile = [&cacheDir, &manifestPath, &assetFilePath, &loadSourceFileFn, &saveBinaryCacheFn]() -> std::expected<AssetManifest, std::error_code>
	{
		ZoneScopedN("LoadAsset::importSourceFile");

		auto uuid = uuids::uuid_system_generator{}();
		auto uuidStr = uuids::to_string(uuid);

		std::error_code error;

		auto parentPath = manifestPath.parent_path();
		auto parentPathStatus = std::filesystem::status(parentPath);
		if (!std::filesystem::exists(parentPathStatus) ||
			!std::filesystem::is_directory(parentPathStatus))
			std::filesystem::create_directories(parentPath, error);

		if (error)
			return std::unexpected(error);

		auto manifestFile = mio_extra::ResizeableMemoryMapSink();
		manifestFile.map(manifestPath.string(), error);

		if (error)
			return std::unexpected(error);

		auto asset = LoadBinary<true>(assetFilePath, loadSourceFileFn);
		if (!asset)
			return std::unexpected(asset.error());

		auto cache = SaveBinary<true>(cacheDir / uuidStr, saveBinaryCacheFn);
		if (!cache)
			return std::unexpected(cache.error());

		AssetManifest manifest{asset.value(), cache.value()};

		glz::write<glz::opts{.prettify = true}>(manifest, manifestFile);

		manifestFile.truncate(manifestFile.Size(), error);

		if (error)
			return std::unexpected(error);

		return manifest;
	};

	std::expected<AssetManifest, AssetManifestError> manifest;

	if (std::filesystem::exists(manifestStatus) && std::filesystem::is_regular_file(manifestStatus))
	{
		ZoneScopedN("LoadAsset::LoadJSONAssetManifest");

		auto manifestFile = mio::mmap_source();
		manifestFile.map(manifestPath.string(), error);

		if (error)
			return std::unexpected(error);

		manifest = LoadJSONAssetManifest<false>(
			std::string_view(manifestFile.cbegin(), manifestFile.cend()),
			[](std::string_view buffer) { return LoadJSONObject<AssetManifest>(buffer); });
	}
	else
	{
		manifest = std::unexpected(AssetManifestErrorCode::kMissing);
	}

	if (!manifest)
	{
		if (std::holds_alternative<AssetManifestErrorCode>(manifest.error()))
			std::cerr << "Asset manifest is invalid: "
					  << ToString(std::get<AssetManifestErrorCode>(manifest.error()))
					  << ", Path: " << manifestPath << '\n';
		else if (std::holds_alternative<std::error_code>(manifest.error()))
			std::cerr << "Asset manifest is invalid: " << std::get<std::error_code>(manifest.error()).message() << ", Path: " << manifestPath << '\n';
		else
			std::cerr << "Asset manifest is invalid: Unknown error, Path: " << manifestPath << '\n';

		std::filesystem::remove(manifestPath, error);
		if (error)
			return std::unexpected(error);
		
		std::cerr << "Reimporting source file\n";

		if (auto result = importSourceFile(); result)
			manifest = result;
		else
			return std::unexpected(result.error());

		return manifest->cacheFileInfo;
	}

	return LoadBinary<false>(manifest->cacheFileInfo.path, loadBinaryCacheFn);
}

} // namespace file
