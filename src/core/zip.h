#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace core
{

class TaskExecutor;

} // namespace core

namespace core::zip
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

constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflated = 8;

// an entry's data, as the local header before it describes it: some archives have stale central directory entries (e.g.
// cube.zip in the McGuire archive, after a file was added to it), which unzip ignores as well
struct EntryData
{
	std::span<const std::byte> compressed; // in the archive's bytes
	uint64_t size = 0;
	uint32_t crc = 0;
	uint16_t method = 0;
};

// lists the entries of a zip archive (zip64 too, but not split over several files). returns an error message if the
// file can't be read or isn't a zip archive.
[[nodiscard]] std::expected<std::vector<Entry>, std::string> List(const std::filesystem::path& archive);

// an entry's data in archive (the archive file's bytes, e.g. core::file::Map's), or why it can't be extracted: a
// corrupt header, encryption, or a compression method other than stored and deflated
[[nodiscard]] std::expected<EntryData, std::string> Locate(std::span<const std::byte> archive, const Entry& entry);

// decompresses raw deflate data (as zip entries hold) into out, which must be exactly the decompressed size (libdeflate)
[[nodiscard]] bool Inflate(std::span<const std::byte> compressed, std::span<std::byte> out);

// the crc-32 zip archives check their entries with (libdeflate's, using the cpu's crc instructions)
[[nodiscard]] uint32_t Crc32(std::span<const std::byte> bytes) noexcept;

// extracts the files of a zip archive into directory (creating it), skipping directories and macOS resource forks
// (__MACOSX/, ._*). refuses entries whose names would land outside directory, and checks every file's crc. the entries
// are inflated on executor's threads if there is one (waiting with TaskExecutor::Join, so this may be called from a
// task), else on this one. progress is advanced from 0 to 255 by the bytes extracted. returns an error message if
// anything fails, or if cancelled() returns true, possibly leaving some files extracted.
[[nodiscard]] std::expected<void, std::string> ExtractAll(
	const std::filesystem::path& archive,
	const std::filesystem::path& directory,
	TaskExecutor* executor = nullptr,
	std::atomic_uint8_t* progress = nullptr,
	const std::function<bool()>& cancelled = {});

} // namespace core::zip
