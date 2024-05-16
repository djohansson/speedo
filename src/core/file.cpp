#include "file.h"

#include <ctime>
#include <chrono>

namespace file
{

namespace detail
{

const char* ToString(AssetManifestErrorCode code)
{
	switch (code)
	{
	case AssetManifestErrorCode::kMissing: return "Missing";
	case AssetManifestErrorCode::kInvalidVersion: return "InvalidVersion";
	case AssetManifestErrorCode::kInvalidLocation: return "InvalidLocation";
	case AssetManifestErrorCode::kInvalidSourceFile: return "InvalidSourceFile";
	case AssetManifestErrorCode::kInvalidCacheFile: return "InvalidCacheFile";
	}

	return "Unknown";
}

} // namespace detail

std::string GetTimeStamp(const std::filesystem::path& filePath)
{
	ZoneScoped;

	std::time_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::filesystem::last_write_time(filePath).time_since_epoch())).count();
	
	static constexpr size_t kBufferSize = 80;
	std::array<char, kBufferSize> buffer;

	return {buffer.data(), std::strftime(buffer.data(), buffer.size(), "%c", std::localtime(&timestamp))};
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
