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
	case AssetManifestErrorCode::kInvalidVersion: return "InvalidVersion";
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

} // namespace file
