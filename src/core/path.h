#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

// plain filesystem helpers, kept apart from core/file.h so that their users don't pull in mio (and through it, on
// windows, <windows.h>).

namespace core
{

namespace file
{

[[nodiscard]] std::expected<std::string, std::error_code> GetTimeStamp(const std::filesystem::path& filePath) noexcept;

[[nodiscard]] std::expected<std::filesystem::path, std::error_code> GetCanonicalPath(
	const char* pathStr,
	const char* defaultPathStr,
	bool createIfMissing = false) noexcept;

} // namespace file

} // namespace core
