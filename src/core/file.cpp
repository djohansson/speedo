#include "file.h"

#include <ctime>
#include <chrono>

namespace file
{

namespace detail
{

const char* to_string(AssetManifestErrorCode code)
{
	switch (code)
	{
	case AssetManifestErrorCode::Missing: return "Missing";
	case AssetManifestErrorCode::InvalidVersion: return "InvalidVersion";
	case AssetManifestErrorCode::InvalidLocation: return "InvalidLocation";
	case AssetManifestErrorCode::InvalidSourceFile: return "InvalidSourceFile";
	case AssetManifestErrorCode::InvalidCacheFile: return "InvalidCacheFile";
	}

	return "Unknown";
}

} // namespace detail

std::string GetTimeStamp(const std::filesystem::path& filePath)
{
	ZoneScoped;

	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::filesystem::last_write_time(filePath).time_since_epoch());
	auto s = std::chrono::duration_cast<std::chrono::seconds>(ms);
	std::time_t timestamp = s.count();

	return {std::asctime(std::localtime(&timestamp))};
}

std::filesystem::path
GetCanonicalPath(const char* pathStr, const char* defaultPathStr, bool createIfMissing)
{
	ASSERT(defaultPathStr != nullptr);

	auto path = std::filesystem::path((pathStr != nullptr) ? pathStr : defaultPathStr);

	if (createIfMissing && !std::filesystem::exists(path))
		std::filesystem::create_directory(path);

	ASSERT(std::filesystem::is_directory(path));

	return std::filesystem::canonical(path);
}

} // namespace file
