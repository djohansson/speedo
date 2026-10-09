#include <core/file.h>

#include <core/application.h>
#include <core/assert.h>//NOLINT(modernize-deprecated-headers)

#include <algorithm>
#include <cstring>
#include <ctime>
#include <chrono>
#include <iostream>

#include <core/uuids_extra.h>

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#endif

namespace core
{

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
	case AssetManifestErrorCode::kInvalidDependencyFile: return "InvalidDependencyFile";
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

	auto* time = std::localtime(&timestamp);

	if (time == nullptr)
		return std::unexpected(std::make_error_code(std::errc::invalid_argument));

	auto timeStrSize = std::strftime(buffer.data(), buffer.size(), "%c", time);

	if (timeStrSize == 0)
		return std::unexpected(std::make_error_code(std::errc::invalid_argument));

	return std::string(buffer.data(), timeStrSize);
}

std::expected<std::filesystem::path, std::error_code>
GetCanonicalPath(const char* pathStr, const char* defaultPathStr, bool createIfMissing) noexcept
{
	ENSURE(defaultPathStr != nullptr);

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
	const std::string& parameterHash,
	const DependenciesFn& dependenciesFn,
	std::atomic_uint8_t* progressOut,
	const std::function<bool()>& cancelled)
{
	using namespace detail;
	
	ZoneScoped;

	auto rootPath = std::get<std::filesystem::path>(Application::Get()->GetEnv().variables["RootPath"]);
	auto cacheDir = std::get<std::filesystem::path>(Application::Get()->GetEnv().variables["UserProfilePath"]);
	auto cacheDirStatus = std::filesystem::status(cacheDir);
	if (!std::filesystem::exists(cacheDirStatus) ||
		!std::filesystem::is_directory(cacheDirStatus))
		std::filesystem::create_directories(cacheDir);

	std::error_code error;
	auto relativePath = std::filesystem::relative(assetFilePath, rootPath, error);
	if (error)
		return std::unexpected(error);

	// assets outside the root (e.g. opened from anywhere with the file dialog) would otherwise put their manifest
	// outside the cache directory, so they go under external/ by their absolute path
	if (relativePath.empty() || *relativePath.begin() == "..")
		relativePath = std::filesystem::path("external") / std::filesystem::absolute(assetFilePath, error).relative_path();
	if (error)
		return std::unexpected(error);

	std::filesystem::path manifestPath(cacheDir / relativePath);

	manifestPath /= parameterHash + ".manifest.bin";

	auto manifestStatus = std::filesystem::status(manifestPath);

	// importing reports progress (0-255) as: hashing the source file up to kImportBegin, then the import itself
	// (loadSourceFileFn and saveBinaryCacheFn, which should leave room for the rest), then hashing the cache file it saved
	// up to kCacheHashEnd. loading a cached asset leaves all of it to loadBinaryCacheFn.
	static constexpr uint8_t kImportBegin = 32;
	static constexpr uint8_t kCacheHashEnd = 255;

	auto importSourceFile = [&cacheDir, &manifestPath, &assetFilePath, &loadSourceFileFn, &saveBinaryCacheFn, &dependenciesFn, progressOut, &cancelled]() -> std::expected<AssetManifest, std::error_code>
	{
		ZoneScopedN("LoadAsset::importSourceFile");

		auto uuid = uuids::NewUuid();
		auto uuidStr = uuids::to_string(uuid);

		std::error_code error;

		auto parentPath = manifestPath.parent_path();
		auto parentPathStatus = std::filesystem::status(parentPath);
		if (!std::filesystem::exists(parentPathStatus) ||
			!std::filesystem::is_directory(parentPathStatus))
			std::filesystem::create_directories(parentPath, error);

		if (error)
			return std::unexpected(error);

		auto manifestFile = mio_extra::resizeable_mmap_sink<std::byte>();
		manifestFile.map(manifestPath.string(), error);

		if (error)
			return std::unexpected(error);

		// a failed (e.g. cancelled) import leaves neither an empty manifest nor an orphaned cache file behind
		auto fail = [&manifestFile, &manifestPath, cachePath = cacheDir / uuidStr](std::error_code failure)
		{
			manifestFile.unmap();
			std::error_code ignored;
			std::filesystem::remove(manifestPath, ignored);
			std::filesystem::remove(cachePath, ignored);
			return std::unexpected(failure);
		};

		auto asset = LoadBinary<true>(
			assetFilePath, loadSourceFileFn, {.value = progressOut, .end = kImportBegin, .cancelled = cancelled});
		if (!asset)
			return fail(asset.error());

		auto cache = SaveBinary<true>(
			cacheDir / uuidStr, saveBinaryCacheFn, {.value = progressOut, .end = kCacheHashEnd, .cancelled = cancelled});
		if (!cache)
			return fail(cache.error());

		AssetManifest manifest{.assetFileInfo = asset.value(), .cacheFileInfo=cache.value()};

		if (dependenciesFn)
		{
			for (const auto& dependencyPath : dependenciesFn())
			{
				// a dependency that can't be read can't be checked either, so it is left out rather than failing the load
				if (auto dependency = GetRecord<true>(dependencyPath); dependency)
					manifest.dependencyFileInfos.emplace_back(std::move(dependency.value()));
				else
					std::cerr << "Failed to record asset dependency: " << dependencyPath << '\n';
			}
		}

		auto outStream = zpp::bits::out(manifestFile, zpp::bits::no_fit_size{}, zpp::bits::no_enlarge_overflow{});

		if (auto result = outStream(manifest); failure(result))
			return fail(std::make_error_code(result));

		manifestFile.truncate(outStream.position(), error);

		if (error)
			return fail(error);

		return manifest;
	};

	std::expected<AssetManifest, AssetManifestError> manifest;

	if (std::filesystem::exists(manifestStatus) && std::filesystem::is_regular_file(manifestStatus))
	{
		ZoneScopedN("LoadAsset::LoadAssetManifest");

		auto manifestFile = mio::basic_mmap_source<std::byte>();
		manifestFile.map(manifestPath.string(), error);

		if (error)
			return std::unexpected(error);

		manifest = LoadAssetManifest<false>(
			std::span<const std::byte>(manifestFile.data(), manifestFile.size()),
			[](std::span<const std::byte> buffer) { return LoadObject<AssetManifest>(buffer); });
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

	auto cache = LoadBinary<false>(manifest->cacheFileInfo.path, loadBinaryCacheFn);
	if (cache || cache.error() == std::errc::operation_canceled)
		return cache;

	// e.g. a cache written by an older version with the same layout hash, that can't be read anymore
	std::cerr << "Asset cache is invalid: " << cache.error().message() << ", Path: " << manifest->cacheFileInfo.path << '\n';

	std::filesystem::remove(manifestPath, error);
	std::filesystem::remove(manifest->cacheFileInfo.path, error);

	std::cerr << "Reimporting source file\n";

	if (auto result = importSourceFile(); result)
		return result->cacheFileInfo;
	else
		return std::unexpected(result.error());
}

std::expected<MappedFile, std::error_code> Map(const std::filesystem::path& filePath)
{
	ZoneScoped;

	std::error_code error;
	auto size = std::filesystem::file_size(filePath, error);
	if (error)
		return std::unexpected(error);
	if (size == 0)
		return MappedFile{};

	MappedFile file;
	file.map(filePath.string(), error);
	if (error)
		return std::unexpected(error);

	// read-ahead of the whole file: mapped pages are otherwise faulted in one at a time
#if defined(__APPLE__)
	radvisory advisory{.ra_offset = 0, .ra_count = static_cast<int>(std::min<uint64_t>(size, INT32_MAX))};
	::fcntl(file.file_handle(), F_RDADVISE, &advisory);
#elif defined(__linux__)
	::posix_fadvise(file.file_handle(), 0, 0, POSIX_FADV_WILLNEED);
#endif

	return file;
}

std::expected<void, std::error_code> Write(const std::filesystem::path& filePath, std::span<const std::byte> data)
{
	ZoneScoped;

	std::error_code error;
	{
		mio_extra::resizeable_mmap_sink<std::byte> file;
		file.map(filePath.string(), error); // creates it
		if (error)
			return std::unexpected(error);

		if (!data.empty())
		{
			if (auto resized = file.resize(data.size()); !resized)
				return std::unexpected(resized.error());
			std::memcpy(file.data(), data.data(), data.size());
			file.sync(error);
			if (error)
				return std::unexpected(error);
		}
	}

	// shorter than the file it replaces
	if (std::filesystem::file_size(filePath, error) != data.size() && !error)
		std::filesystem::resize_file(filePath, data.size(), error);
	if (error)
		return std::unexpected(error);

	return {};
}

} // namespace file

} // namespace core
