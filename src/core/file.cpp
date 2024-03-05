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

std::string getTimeStamp(const std::filesystem::path& filePath)
{
	ZoneScoped;

	std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::filesystem::last_write_time(filePath).time_since_epoch());
	std::chrono::seconds s = std::chrono::duration_cast<std::chrono::seconds>(ms);
	std::time_t timestamp = s.count();

	return std::string(std::asctime(std::localtime(&timestamp)));
}

} // namespace file
