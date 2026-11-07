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

// the running executable's path (resolved: through a symlink to it, a macOS bundle's Contents/MacOS/<name>)
[[nodiscard]] std::expected<std::filesystem::path, std::error_code> GetExecutablePath() noexcept;

// where an application keeps its user's data on this platform: ~/Library/Application Support/<name> on macOS,
// %LOCALAPPDATA%\<name> on Windows, $XDG_DATA_HOME/<name> (else ~/.local/share/<name>) elsewhere. not created
[[nodiscard]] std::filesystem::path GetUserDataPath(std::string_view name);

} // namespace file

} // namespace core
