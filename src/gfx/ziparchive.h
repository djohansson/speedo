#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gfx::zip
{

struct Entry
{
	std::string name; // relative path, with forward slashes
	uint64_t compressedSize = 0;
	uint64_t size = 0;
	uint64_t localHeaderOffset = 0;
	uint32_t crc = 0;
	uint16_t method = 0; // 0: stored, 8: deflated (the only ones supported)
	uint16_t flags = 0;

	[[nodiscard]] bool IsDirectory() const noexcept { return name.ends_with('/'); }
};

// lists the entries of a zip archive (zip64 too, but not split over several files). returns an error message if the
// file can't be read or isn't a zip archive.
[[nodiscard]] std::expected<std::vector<Entry>, std::string> List(const std::filesystem::path& archive);

// extracts the files of a zip archive into directory (creating it), skipping directories and macOS resource forks
// (__MACOSX/, ._*). refuses entries whose names would land outside directory, and checks every file's crc. progress is
// advanced from 0 to 255 by the bytes extracted. returns an error message if anything fails, or if cancelled() returns
// true, possibly leaving some files extracted.
[[nodiscard]] std::expected<void, std::string> ExtractAll(
	const std::filesystem::path& archive,
	const std::filesystem::path& directory,
	std::atomic_uint8_t* progress = nullptr,
	const std::function<bool()>& cancelled = {});

} // namespace gfx::zip
